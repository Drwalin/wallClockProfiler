#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <sys/prctl.h>

#include <ctime>

#include "SimpleVector.hpp"
#include "StringUtil.hpp"

static void usage()
{
	printf("\nDirect call usage:\n\n"
		   "    wallClockProfiler samples_per_sec ./myProgram\n\n");
	printf("Attach to existing process (may require root):\n\n"
		   "    wallClockProfiler samples_per_sec ./myProgram pid "
		   "[detatch_sec]\n\n");
	printf("detatch_sec is the (optional) number of seconds before detatching "
		   "and\n"
		   "ending profiling (or -1 to stay attached forever, default)\n\n");

	exit(1);
}

int inPipe;
int outPipe;

char sendBuff[1024];

FILE *logFile = NULL;

static void log(const char *inHeader, char *inBody)
{
	if (logFile != NULL) {
		fprintf(logFile, "%s:\n%s\n\n\n", inHeader, inBody);
		fflush(logFile);
	}
}

static void sendCommand(const char *inCommand)
{
	log("Sending command to GDB", (char *)inCommand);

	sprintf(sendBuff, "%s\n", inCommand);
	write(outPipe, sendBuff, strlen(sendBuff));
}

// 65 KiB buffer
// if GDB issues a single response that is longer than this
// we will only return or processes the tail end of it.
#define READ_BUFF_SIZE 65536
char readBuff[READ_BUFF_SIZE];

char anythingInReadBuff = false;
char numReadAttempts = 0;

#define BUFF_TAIL_SIZE 32768
char tailBuff[BUFF_TAIL_SIZE];

char programExited = false;
char detatchJustSent = false;

static int fillBufferWithResponse(const char *inWaitingFor = NULL)
{
	int readSoFar = 0;
	anythingInReadBuff = false;
	numReadAttempts = 0;

	while (true) {

		if (readSoFar >= READ_BUFF_SIZE - 1) {
			// we've filled up our read buffer

			// save the last bit of it, but discard the rest

			// copy end, including last \0
			memcpy(tailBuff, &(readBuff[readSoFar + 1 - BUFF_TAIL_SIZE]),
				   BUFF_TAIL_SIZE);

			memcpy(readBuff, tailBuff, BUFF_TAIL_SIZE);

			readSoFar = BUFF_TAIL_SIZE - 1;
		}

		numReadAttempts++;

		int numRead = read(inPipe, &(readBuff[readSoFar]),
						   (READ_BUFF_SIZE - 1) - readSoFar);

		if (numRead > 0) {
			anythingInReadBuff = true;

			readSoFar += numRead;

			readBuff[readSoFar] = '\0';

			if (strstr(readBuff, "(gdb)") != NULL &&
				(inWaitingFor == NULL ||
				 strstr(readBuff, inWaitingFor) != NULL)) {
				// read full response
				return readSoFar;
			} else if (readSoFar > 10 && !detatchJustSent &&
					   strstr(readBuff, "thread-group-exited") != NULL) {
				// stop waiting for full response, program has exited
				programExited = true;
				return readSoFar;
			}
		} else if (numRead == -1) {
			if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
				char *errorString = strerror(errno);
				printf("Error in reading from GDB pipe: %s\n", errorString);
				return readSoFar;
			} else {
				// sleep to avoid CPU starving pipe sender
				usleep(200);
			}
		}
	}
}

static void checkProgramExited()
{
	if (anythingInReadBuff) {
		if (strstr(readBuff, "exited-normally") != NULL) {
			programExited = true;

			log("Detected program exit:\n"
				"GDB response contains 'exited-normally'",
				readBuff);
		} else if (strstr(readBuff, "\"exited\"") != NULL) {
			programExited = true;

			log("Detected program exit:\n"
				"GDB response contains '\"exited\"'",
				readBuff);
		} else if (strstr(readBuff, "stopped") != NULL &&
				   strstr(readBuff, "signal-received") != NULL &&
				   strstr(readBuff, "SIGINT") == NULL) {

			programExited = true;

			log("Detected program exit:\n"
				"GDB response shows that we stopped "
				"with a signal other than SIGINT",
				readBuff);
		}
	}
}

[[maybe_unused]] static void printGDBResponse()
{
	int numRead = fillBufferWithResponse();

	if (numRead > 0) {
		checkProgramExited();
		printf("\n\nRead from GDB:  %s", readBuff);
	}
}

[[maybe_unused]] static void printGDBResponseToFile(FILE *inFile)
{
	int numRead = fillBufferWithResponse();

	if (numRead > 0) {
		checkProgramExited();
		fprintf(inFile, "\n\nRead from GDB:  %s", readBuff);
	}
}

static void skipGDBResponse()
{
	[[maybe_unused]] int numRead = fillBufferWithResponse();

	if (anythingInReadBuff) {
		log("Skipping GDB response", readBuff);
	}

	checkProgramExited();
}

static void waitForGDBInterruptResponse()
{
	[[maybe_unused]] int numRead = fillBufferWithResponse("*stopped,");

	if (anythingInReadBuff) {
		log("Waiting for interrupt response", readBuff);
	}

	checkProgramExited();
}

static char *getGDBResponse()
{
	int numRead = fillBufferWithResponse();
	checkProgramExited();

	char *val;
	if (numRead == 0) {
		val = stringDuplicate("");
	} else {
		val = stringDuplicate(readBuff);
	}

	log("getGDBResponse returned", val);

	return val;
}

typedef struct StackFrame {
	void *address;
	char *funcName;
	char *fileName;
	int lineNum;
} StackFrame;

typedef struct Stack {
	SimpleVector<StackFrame> frames;
	int sampleCount;
} Stack;

typedef struct FunctionRecord {
	char *funcName;
	int sampleCount;
} FunctionRecord;

SimpleVector<Stack> stackLog;

// these are for counting repeated common stack roots
// they do NOT need to be freed (they cointain poiters to strings
// in the main stack log)
#define NUM_ROOT_STACKS_TO_TRACK 15
SimpleVector<Stack> stackRootLog[NUM_ROOT_STACKS_TO_TRACK];

// does not make sense to call this unless depth less than full stack depth
Stack getRoot(Stack inFullStack, int inDepth)
{
	Stack newStack;
	newStack.sampleCount = 1;
	int numToSkip = inFullStack.frames.size() - inDepth;

	for (int i = numToSkip; i < inFullStack.frames.size(); i++) {
		newStack.frames.push_back(inFullStack.frames.getElementDirect(i));
	}

	return newStack;
}

static char stackCompare(Stack *inA, Stack *inB)
{
	if (inA->frames.size() != inB->frames.size()) {
		return false;
	}
	for (int i = 0; i < inA->frames.size(); i++) {
		if (inA->frames.getElementDirect(i).address !=
			inB->frames.getElementDirect(i).address) {
			return false;
		}
	}
	return true;
}

static void freeStack(Stack *inStack)
{
	for (int i = 0; i < inStack->frames.size(); i++) {
		StackFrame f = inStack->frames.getElementDirect(i);
		delete[] f.funcName;
		delete[] f.fileName;
	}
	inStack->frames.deleteAll();
}

static StackFrame parseFrame(char *inFrameString)
{
	StackFrame newF;

	char *openPos = strstr(inFrameString, "{");

	if (openPos == NULL) {
		printf("Error parsing stack frame:  %s\n", inFrameString);
		exit(1);
	}
	openPos = &(openPos[1]);

	char *closePos = strstr(openPos, "}");

	if (closePos == NULL) {
		printf("Error parsing stack frame:  %s\n", inFrameString);
		exit(1);
	}
	closePos[0] = '\0';

	int numVals;
	char **vals = split(openPos, ",", &numVals);

	void *address = NULL;

	for (int i = 0; i < numVals; i++) {
		if (strstr(vals[i], "addr=") == vals[i]) {
			sscanf(vals[i], "addr=\"%p\"", &address);
			break;
		}
	}

	newF.address = address;
	newF.lineNum = -1;
	newF.funcName = NULL;
	newF.fileName = NULL;

	for (int i = 0; i < numVals; i++) {
		if (strstr(vals[i], "func=") == vals[i]) {
			newF.funcName = new char[500];
			sscanf(vals[i], "func=\"%499s\"", newF.funcName);
		} else if (strstr(vals[i], "file=") == vals[i]) {
			newF.fileName = new char[500];
			sscanf(vals[i], "file=\"%499s\"", newF.fileName);
		} else if (strstr(vals[i], "line=") == vals[i]) {
			sscanf(vals[i], "line=\"%d\"", &newF.lineNum);
		}
	}

	if (newF.fileName == NULL) {
		newF.fileName = stringDuplicate("");
	}
	if (newF.funcName == NULL) {
		newF.funcName = stringDuplicate("");
	}

	char *quotePos = strstr(newF.fileName, "\"");
	if (quotePos != NULL) {
		quotePos[0] = '\0';
	}
	quotePos = strstr(newF.funcName, "\"");
	if (quotePos != NULL) {
		quotePos[0] = '\0';
	}

	for (int i = 0; i < numVals; i++) {
		delete[] vals[i];
	}
	delete[] vals;

	return newF;
}

static void logGDBStackResponse()
{
	int numRead = fillBufferWithResponse();

	if (numRead == 0) {
		return;
	}

	log("logGDBStackResponse sees", readBuff);

	checkProgramExited();

	if (programExited) {
		return;
	}

	const char *stackStartMarker = ",stack=[";

	char *stackStartPos = strstr(readBuff, ",stack=[");

	if (stackStartPos == NULL) {
		return;
	}

	char *stackStart = &(stackStartPos[strlen(stackStartMarker)]);

	char *closeBracket = strstr(stackStart, "]\n");

	if (closeBracket == NULL) {
		return;
	}

	// terminate at close
	closeBracket[0] = '\0';

	const char *frameMarker = "frame=";

	if (strstr(stackStart, frameMarker) != stackStart) {
		return;
	}

	// skip first
	stackStart = &(stackStart[strlen(frameMarker)]);

	int numFrames;
	char **frames = split(stackStart, frameMarker, &numFrames);

	Stack thisStack;
	thisStack.sampleCount = 1;
	for (int i = 0; i < numFrames; i++) {
		thisStack.frames.push_back(parseFrame(frames[i]));
		delete[] frames[i];
	}
	delete[] frames;

	char match = false;
	Stack insertedStack = thisStack;

	for (int i = 0; i < stackLog.size(); i++) {
		Stack *inOld = stackLog.getElement(i);

		if (stackCompare(inOld, &thisStack)) {
			match = true;
			inOld->sampleCount++;
			insertedStack = *inOld;
			break;
		}
	}

	if (match) {
		freeStack(&thisStack);
	} else {
		stackLog.push_back(thisStack);
	}

	// now look at roots of inserted stack
	for (int i = 1;
		 i < insertedStack.frames.size() && i < NUM_ROOT_STACKS_TO_TRACK; i++) {

		Stack rootStack = getRoot(insertedStack, i);

		match = false;

		for (int r = 0; r < stackRootLog[i].size(); r++) {
			Stack *inOld = stackRootLog[i].getElement(r);

			if (stackCompare(inOld, &rootStack)) {
				match = true;
				inOld->sampleCount++;
				break;
			}
		}

		if (!match) {
			stackRootLog[i].push_back(rootStack);
		}
	}
}

void printStack(Stack inStack, int inNumTotalSamples)
{
	Stack s = inStack;

	printf("%7.3f%% ===================================== (%d samples)\n"
		   "       %3d: %s   (at %s:%d)\n",
		   100 * s.sampleCount / (float)inNumTotalSamples, s.sampleCount, 1,
		   s.frames.getElement(0)->funcName, s.frames.getElement(0)->fileName,
		   s.frames.getElement(0)->lineNum);

	StackFrame *sf = inStack.frames.getElement(0);

	if (sf->lineNum > 0) {

		char *listCommand = autoSprintf("list %s:%d,%d", sf->fileName,
										sf->lineNum, sf->lineNum);
		sendCommand(listCommand);

		delete[] listCommand;

		char *response = getGDBResponse();

		char *marker = autoSprintf("~\"%d\\t", sf->lineNum);

		char *markerSpot = strstr(response, marker);

		// if name present in line, it's a not-found error
		if (markerSpot != NULL && strstr(markerSpot, sf->fileName) == NULL) {
			char *lineStart = &(markerSpot[strlen(marker)]);

			// trim spaces from start
			while (lineStart[0] == ' ') {
				lineStart = &(lineStart[1]);
			}

			char *lineEnd = strstr(lineStart, "\\n");
			if (lineEnd != NULL) {
				lineEnd[0] = '\0';
			}
			printf("            %d:|   %s\n", sf->lineNum, lineStart);
		}

		delete[] marker;
		delete[] response;
	}

	// print stack for context below
	for (int j = 1; j < s.frames.size(); j++) {
		StackFrame f = s.frames.getElementDirect(j);
		printf("       %3d: %s   (at %s:%d)\n", j + 1, f.funcName, f.fileName,
			   f.lineNum);
	}
	printf("\n\n");
}

int main(int inNumArgs, char **inArgs)
{

	if (inNumArgs != 3 && inNumArgs != 4 && inNumArgs != 5) {
		usage();
	}

	float samplesPerSecond = 100;

	sscanf(inArgs[1], "%f", &samplesPerSecond);

	int readPipe[2];
	int writePipe[2];

	pipe(readPipe);
	pipe(writePipe);

	char *progName = stringDuplicate(inArgs[2]);
	char *progArgs = stringDuplicate("");

	char *spacePos = strstr(progName, " ");

	if (spacePos != NULL) {
		delete[] progArgs;
		progArgs = stringDuplicate(&(spacePos[1]));
		// cut off name at start of args
		spacePos[0] = '\0';
	}

	int childPID = fork();

	if (childPID == -1) {
		printf("Failed to fork\n");

		delete[] progName;
		delete[] progArgs;

		return 1;
	} else if (childPID == 0) {
		// child
		dup2(writePipe[0], STDIN_FILENO);
		dup2(readPipe[1], STDOUT_FILENO);
		dup2(readPipe[1], STDERR_FILENO);

		while (false && true) {
			printf("test\n");
		}

		// ask kernel to deliver SIGTERM in case the parent dies
		prctl(PR_SET_PDEATHSIG, SIGTERM);

		execlp("gdb", "gdb", "-nx", "--interpreter=mi", progName, NULL);

		delete[] progName;
		delete[] progArgs;

		exit(0);
	}

	// else parent
	printf("Forked GDB child on PID=%d\n", childPID);

	logFile = fopen("wcGDBLog.txt", "w");

	printf("Logging GDB commands and responses to wcGDBLog.txt\n");

	// close unused pipe ends
	close(writePipe[0]);
	close(readPipe[1]);

	inPipe = readPipe[0];
	outPipe = writePipe[1];

	fcntl(inPipe, F_SETFL, O_NONBLOCK);

	char *gdbInitResponse = getGDBResponse();

	if (strstr(gdbInitResponse, "No such file or directory.") != NULL) {
		delete[] gdbInitResponse;
		printf("GDB failed to start program '%s'\n", progName);
		fclose(logFile);
		logFile = NULL;
		delete[] progName;
		delete[] progArgs;
		exit(0);
	}
	delete[] gdbInitResponse;

	sendCommand("handle SIGPIPE nostop noprint pass");

	skipGDBResponse();

	if (inNumArgs == 3) {
		char *runCommand = autoSprintf("run %s > wcOut.txt", progArgs);

		printf("\n\nStarting gdb program with '%s', "
			   "redirecting program output to wcOut.txt\n",
			   runCommand);

		sendCommand(runCommand);
		delete[] runCommand;
	} else {
		sendCommand("-gdb-set target-async 1");
		skipGDBResponse();

		printf("\n\nAttaching to PID %s\n", inArgs[3]);

		char *command = autoSprintf("-target-attach %s\n", inArgs[3]);

		sendCommand(command);

		delete[] command;

		char *gdbAttachResponse = getGDBResponse();

		if (strstr(gdbAttachResponse, "ptrace: No such process.") != NULL) {
			delete[] gdbAttachResponse;
			printf("GDB could not find process:  %s\n", inArgs[3]);
			fclose(logFile);
			logFile = NULL;
			delete[] progName;
			delete[] progArgs;
			exit(0);
		} else if (strstr(gdbAttachResponse,
						  "ptrace: Operation not permitted.") != NULL) {
			delete[] gdbAttachResponse;
			printf("GDB could not attach to process %s "
				   "(maybe you need to be root?)\n",
				   inArgs[3]);
			fclose(logFile);
			logFile = NULL;
			delete[] progName;
			delete[] progArgs;
			exit(0);
		}

		delete[] gdbAttachResponse;

		printf("\n\nResuming attached gdb program with '-exec-continue'\n");

		sendCommand("-exec-continue");
	}

	delete[] progArgs;

	usleep(100000);

	skipGDBResponse();

	printf("Debugging program '%s'\n", inArgs[2]);

	//     char rawProgramName[100];

	char *endOfPath = strrchr(inArgs[2], '/');

	char *fullProgName = progName;

	if (endOfPath != NULL) {
		progName = &(endOfPath[1]);
	}

	char *pidCall = autoSprintf("pidof %s", progName);

	delete[] fullProgName;

	FILE *pidPipe = popen(pidCall, "r");

	delete[] pidCall;

	if (pidPipe == NULL) {
		printf("Failed to open pipe to pidof to get debugged app pid\n");
		fclose(logFile);
		logFile = NULL;
		return 1;
	}

	int pid = -1;

	// if there are multiple GDP procs, they are printed in newest-first order
	// this will get the pid of the latest one (our GDB child)
	int numRead = fscanf(pidPipe, "%d", &pid);

	pclose(pidPipe);

	if (numRead != 1) {
		printf("Failed to read PID of debugged app\n");
		fclose(logFile);
		logFile = NULL;
		return 1;
	}

	printf("PID of debugged process = %d\n", pid);

	printf("Sampling stack while program runs...\n");

	int numSamples = 0;

	int usPerSample = lrint(1000000 / samplesPerSecond);

	printf("Sampling %.2f times per second, for %d usec between samples\n",
		   samplesPerSecond, usPerSample);

	time_t startTime = time(NULL);

	int detatchSeconds = -1;

	if (inNumArgs == 5) {
		sscanf(inArgs[4], "%d", &detatchSeconds);
	}
	if (detatchSeconds != -1) {
		printf("Will detatch automatically after %d seconds\n", detatchSeconds);
	}

	while (!programExited &&
		   (detatchSeconds == -1 || time(NULL) < startTime + detatchSeconds)) {
		usleep(usPerSample);

		// interrupt
		if (inNumArgs == 3) {
			// we ran our program with run above to redirect output
			// thus -exec-interrupt won't work
			log("Sending SIGINT to target process", inArgs[2]);

			kill(pid, SIGINT);
		} else {
			sendCommand("-exec-interrupt");
		}

		waitForGDBInterruptResponse();

		if (!programExited) {
			// sample stack
			sendCommand("-stack-list-frames");
			logGDBStackResponse();
			numSamples++;
		}

		if (!programExited) {
			// continue running

			sendCommand("-exec-continue");
			skipGDBResponse();
		}
	}

	if (programExited) {
		printf("Program exited normally\n");
	} else {
		printf("Detatching from program\n");

		if (inNumArgs == 3) {
			// we ran our program with run above to redirect output
			// thus -exec-interrupt won't work
			log("Sending SIGINT to target process", inArgs[2]);

			kill(pid, SIGINT);
		} else {
			sendCommand("-exec-interrupt");
		}
		waitForGDBInterruptResponse();

		detatchJustSent = true;
		sendCommand("-target-detach");
		skipGDBResponse();

		detatchJustSent = false;
	}

	printf("%d stack samples taken\n", numSamples);

	printf("%d unique stacks sampled\n", stackLog.size());

	SimpleVector<FunctionRecord> functions;

	for (int i = 0; i < stackLog.size(); i++) {
		Stack *s = stackLog.getElement(i);

		int sampleCount = s->sampleCount;

		for (int f = 0; f < s->frames.size(); f++) {
			char *funcName = s->frames.getElement(f)->funcName;

			char found = false;
			for (int fdup = 0; fdup < f; fdup++) {
				char *dupFuncName = s->frames.getElement(fdup)->funcName;
				if (strcmp(funcName, dupFuncName) == 0) {
					found = true;
					break;
				}
			}
			if (found) {
				continue;
			}

			found = false;
			for (int r = 0; r < functions.size(); r++) {
				if (strcmp(functions.getElement(r)->funcName, funcName) == 0) {
					// hit
					found = true;
					functions.getElement(r)->sampleCount += sampleCount;
					break;
				}
			}
			if (!found) {
				FunctionRecord newFunc = {funcName, sampleCount};
				functions.push_back(newFunc);
			}
		}
	}

	SimpleVector<FunctionRecord> sortedFunctions;
	while (functions.size() > 0) {
		int max = 1;
		FunctionRecord maxFunc;
		int maxInd = -1;
		for (int i = 0; i < functions.size(); i++) {
			FunctionRecord r = functions.getElementDirect(i);

			if (r.sampleCount > max) {
				maxFunc = r;
				max = r.sampleCount;
				maxInd = i;
			}
		}
		if (maxInd >= 0) {
			sortedFunctions.push_back(maxFunc);
			functions.deleteElement(maxInd);
		} else {
			break;
		}
	}

	// simple insertion sort
	SimpleVector<Stack> sortedStacks;

	while (stackLog.size() > 0) {
		int max = 0;
		Stack maxStack;
		int maxInd = -1;
		for (int i = 0; i < stackLog.size(); i++) {
			Stack s = stackLog.getElementDirect(i);

			if (s.sampleCount > max) {
				maxStack = s;
				max = s.sampleCount;
				maxInd = i;
			}
		}
		if (maxInd >= 0) {
			sortedStacks.push_back(maxStack);
			stackLog.deleteElement(maxInd);
		} else {
			break;
		}
	}

	SimpleVector<Stack> sortedRootStacks[NUM_ROOT_STACKS_TO_TRACK];

	for (int r = 1; r < NUM_ROOT_STACKS_TO_TRACK; r++) {

		while (stackRootLog[r].size() > 0) {
			int max = 1;
			Stack maxStack;
			int maxInd = -1;
			for (int i = 0; i < stackRootLog[r].size(); i++) {
				Stack s = stackRootLog[r].getElementDirect(i);

				if (s.sampleCount > max) {
					maxStack = s;
					max = s.sampleCount;
					maxInd = i;
				}
			}
			if (maxInd >= 0) {
				sortedRootStacks[r].push_back(maxStack);
				stackRootLog[r].deleteElement(maxInd);
			} else {
				break;
			}
		}
	}

	printf("\n\n\nReport:\n\n");

	printf("\n\n\nFunctions "
		   "with more than one sample:\n\n");

	for (int i = 0; i < sortedFunctions.size(); i++) {
		FunctionRecord f = sortedFunctions.getElementDirect(i);

		printf("%7.3f%% ===================================== (%d samples)\n"
			   "         %s\n\n\n",
			   100 * f.sampleCount / (float)numSamples, f.sampleCount,
			   f.funcName);
	}

	for (int r = 1; r < NUM_ROOT_STACKS_TO_TRACK; r++) {
		if (sortedRootStacks[r].size() > 0) {

			printf("\n\n\nPartial stacks of depth [%d] "
				   "with more than one sample:\n\n",
				   r);

			for (int i = 0; i < sortedRootStacks[r].size(); i++) {
				Stack s = sortedRootStacks[r].getElementDirect(i);
				printStack(s, numSamples);
			}
		}
	}

	printf("\n\n\nFull stacks "
		   "with at least one sample:\n\n");

	for (int i = 0; i < sortedStacks.size(); i++) {
		Stack s = sortedStacks.getElementDirect(i);
		printStack(s, numSamples);

		freeStack(&s);
	}

	// stacks that had only one sample
	for (int i = 0; i < stackLog.size(); i++) {
		Stack s = stackLog.getElementDirect(i);
		freeStack(&s);
	}

	fclose(logFile);
	logFile = NULL;

	return 0;
}

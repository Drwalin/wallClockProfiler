#pragma once`

#ifndef WCPROF_MAIN_CPP
#define WCPROF_MAIN_CPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>

#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <signal.h>

std::string argv0 = "wcprof";

static void Usage()
{
	printf("Usage to attach to a running process:\n");
	printf("    %s attach [sample_method] [delay] [time] [pid] [executable]\n", argv0.c_str());
	printf("Usage to attach to run a program:\n");
	printf("    %s run [sample_method] [delay] [time] [executable] [optional program args...]\n", argv0.c_str());
	printf(" [sample_method] - stack frame sampling method: default, single_thread,\n"
		   "                   all_threads, round_robin_N ;\n"
		   "                   where N is maximum number of threads per single sampling ;\n"
		   "                   default is the same as single_thread\n");
	printf(" [delay] - microseconds between each stack sample\n");
	printf(" [time] - seconds to profile program for (if 0 or less, then profile until program is running)\n");
	printf(" [pid] - PID of a program\n");
	printf(" [executable] - path to a executable\n");
	printf("Each argument is mandatory.\n");
}

static void Sleep(int64_t us)
{
	std::this_thread::sleep_for(std::chrono::microseconds(us));
}

bool programFinished = false;

int inPipe = 0, outPipe = 0;

std::string gdbResponse;

static const std::string &FetchGdbResponse(std::string end)
{
	static std::string nextData;
	gdbResponse = nextData;
	nextData.clear();
	for (int i = 0; i < 180 * 1000 * 5; ++i) {
		const size_t oldSize = gdbResponse.size();
		const size_t extension = 1024 * 64;
		gdbResponse.resize(oldSize + extension);
		const int numRead =
			read(inPipe, gdbResponse.data() + oldSize, extension);
		if (numRead > 0) {
			const size_t newSize = oldSize + numRead;
			gdbResponse[newSize] = 0;
			gdbResponse.resize(newSize);

			if (end != "") {
				auto endPos = gdbResponse.find(end);
				if (endPos != std::string::npos) {
					size_t newSize = endPos + end.size();
					nextData = std::string(gdbResponse.c_str() + newSize);
					gdbResponse[newSize] = 0;
					gdbResponse.resize(newSize);
					return gdbResponse;
				}
			}

			if (gdbResponse.find("[Inferior") != std::string::npos) {
				if (gdbResponse.find("exited") != std::string::npos) {
					programFinished = true;
					return gdbResponse;
				}
			}

			if (gdbResponse.find(
					"Program terminated with signal SIGKILL, Killed.") !=
				std::string::npos) {
				programFinished = true;
				return gdbResponse;
			}
			if (gdbResponse.find(
					"Program terminated with signal SIGTERM, Terminated.") !=
				std::string::npos) {
				programFinished = true;
				return gdbResponse;
			}
			if (gdbResponse.find(
					"Program received signal SIGSEGV, Segmentation fault.") !=
				std::string::npos) {
				programFinished = true;
				return gdbResponse;
			}
			if (gdbResponse.find("The program is not being run.") !=
				std::string::npos) {
				programFinished = true;
				return gdbResponse;
			}

		} else if (numRead == -1) {
			if (end != "") {
				auto endPos = gdbResponse.find(end);
				if (endPos != std::string::npos) {
					size_t newSize = endPos + end.size();
					nextData = std::string(gdbResponse.c_str() + newSize);
					gdbResponse[newSize] = 0;
					gdbResponse.resize(newSize);
					return gdbResponse;
				}
			}

			gdbResponse.resize(oldSize);
			if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
				char *errorString = strerror(errno);
				printf("Error in reading from GDB pipe: %s\n", errorString);
				raise(SIGTERM);
				return gdbResponse;
			} else {
				Sleep(200);
			}
		} else {
			Sleep(200);
		}
	}
	return gdbResponse;
}

static void SendCommand(std::string com)
{
	com += "\n";
	write(outPipe, com.c_str(), com.size());
}

void CalcAndPrintStatisticsAndExit();

void signal_handler_parent(int signal)
{
	SendCommand("interrupt");
	FetchGdbResponse("(gdb)");
	SendCommand("quit");
	FetchGdbResponse("(gdb)");
	CalcAndPrintStatisticsAndExit();
	exit(signal);
}

struct StackFrame {
	std::string threadName;
	std::vector<size_t> stackFrameLines;
};

struct StackFrameLine {
	std::string fullLine;
	std::string functionName;
};

size_t totalCapturedFramesCount = 0;
std::vector<StackFrameLine> individualStackFrameLines;
std::map<std::string, size_t> mapIndividualStackFrameLinesToId;
std::map<std::string, std::vector<StackFrame>> capturedStackFramesPerThread;

#include "sampling_methods.hpp"

int main(int argc, char **argv)
{
	argv0 = argv[0];
	if (argc < 6) {
		Usage();
		return 1;
	}

	enum SamplingMethods {
		SINGLE_THREAD,
		ALL_THREADS,
		ROUND_ROBIN,
	} samplingMethod;

	int64_t microsecondsDelay = atoll(argv[3]);
	int maxRoundRobinThreads = 1;
	bool attach = false;
	std::string programName;
	std::string programArguments;
	std::string pid = "";
	int64_t profilingTime = 0;

	{
		std::string m = argv[2];
		if (m == "single_thread") {
			samplingMethod = SINGLE_THREAD;
		} else if (m == "all_threads") {
			samplingMethod = ALL_THREADS;
		} else {
			std::string rr = "round_robin_";
			if (m.find(rr) == 0) {
				samplingMethod = ROUND_ROBIN;
				sscanf(m.data() + rr.size(), "%i", &maxRoundRobinThreads);
				if (maxRoundRobinThreads <= 0 || maxRoundRobinThreads > 10000) {
					printf("Round robin sampling requires sample count of 1 to "
						   "10000\n");
					Usage();
					return 1;
				}
			} else {
				printf("Invalid sampling method: '%s'\n", argv[2]);
				Usage();
				return 1;
			}
		}
	}

	if (std::string(argv[1]) == "attach") {
		if (argc < 7) {
			Usage();
			return 1;
		}
		attach = true;
		profilingTime = atoll(argv[4]);
		pid = argv[5];
		programName = argv[6];
	} else if (std::string(argv[1]) == "run") {
		attach = false;
		programName = argv[4];
		for (int i = 4; i < argc; ++i) {
			programArguments += " \"";
			programArguments += argv[i];
			programArguments += "\"";
		}
	} else {
		printf("Invalid command: `%s`\n\n", argv[1]);
		Usage();
		return 2;
	}

	int readPipe[2];
	int writePipe[2];
	pipe(readPipe);
	pipe(writePipe);

	int childPID = fork();

	if (childPID == -1) {
		printf("Failed to fork\n");

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

		execlp("gdb", "gdb", "-nx", programName.c_str(), nullptr);

		exit(0);
	}

	signal(SIGKILL, signal_handler_parent);
	signal(SIGTERM, signal_handler_parent);

	// else parent
	printf("Forked GDB child on PID=%d\n", childPID);

	// close unused pipe ends
	close(writePipe[0]);
	close(readPipe[1]);

	inPipe = readPipe[0];
	outPipe = writePipe[1];

	fcntl(inPipe, F_SETFL, O_NONBLOCK);

	FetchGdbResponse("(gdb)");

	if (gdbResponse.find("No such file or directory.") != std::string::npos) {
		programFinished = true;
		printf("GDB failed to start program '%s'\n", programName.c_str());
		exit(0);
	}

	SendCommand("handle SIGPIPE nostop noprint pass");

	FetchGdbResponse("(gdb)");

	if (attach == false) {
		SendCommand(std::string("run \"") + programArguments +
					" > wcprof_program_output.txt &");
		FetchGdbResponse("(gdb)");
	} else {
		SendCommand(std::string("attach ") + pid + " &");

		FetchGdbResponse("(gdb)");

		if (gdbResponse.find("ptrace: No such process.") != std::string::npos) {
			printf("GDB could not find process:  %s\n", pid.c_str());
			exit(0);
		}
		if (gdbResponse.find("ptrace: Operation not permitted.") !=
			std::string::npos) {
			printf("GDB could not attach to process %s "
				   "(maybe you need to be root?)\n",
				   pid.c_str());
			exit(0);
		}
	}

	// Fetch true pid of a process, not a thread
	int _pid = -1;
	{
		SendCommand("interrupt");
		SendCommand("interrupt");
		SendCommand("info inferior");
		SendCommand("c &");

		FetchGdbResponse("(gdb)"); // interrupt
		FetchGdbResponse("(gdb)"); // interrupt

		FetchGdbResponse("(gdb)"); // info inferior

		auto pos = gdbResponse.find("  process ");
		if (pos == std::string::npos) {
			printf("Cannot fetch process PID\n");
			exit(0);
		} else {
			sscanf(gdbResponse.c_str() + pos + 10, "%i", &_pid);
			pid = std::to_string(_pid);
		}

		FetchGdbResponse("(gdb)"); // c &
	}

	printf("PID of debugged process = %s\n", pid.c_str());
	printf("Sampling stack while program runs...\n");

	time_t startTime = time(nullptr);

	time_t lastReportTime = startTime;

	double stackSamplingSumTime = 0;

	while (!programFinished) {
		time_t now = time(nullptr);
		if (attach) {
			if (now > startTime + profilingTime) {
				break;
			}
		}

		if (lastReportTime + 3 < now) {
			printf("Collected %li stack samples in %li seconds\n",
				   totalCapturedFramesCount, now - startTime);
			lastReportTime = now;
		}

		Sleep(microsecondsDelay);

		// Fetch samples
		auto start = std::chrono::steady_clock::now();

		switch (samplingMethod) {
		case SINGLE_THREAD:
			CollectSingleThreadStackFrame();
			break;
		case ALL_THREADS:
			CollectAllThreadsStackFrames();
			break;
		case ROUND_ROBIN:
			CollectRoundRobinThreadsStackFrames(maxRoundRobinThreads);
			break;
		default:
			printf("Invalid sampling method");
			SendCommand("interrupt");
			FetchGdbResponse("(gdb)");
			SendCommand("quit");
			FetchGdbResponse("(gdb)");
			exit(1);
		}

		auto end = std::chrono::steady_clock::now();
		auto dur = std::chrono::duration<double>(end - start).count();
		stackSamplingSumTime += dur;

		Sleep(microsecondsDelay);
	}

	printf("Average stack sampling duration: %f ms\n",
		   stackSamplingSumTime * 1000.0 / totalCapturedFramesCount);

	if (programFinished) {
		printf("Program exited\n");
	} else {
		if (attach) {
			printf("Detaching from program\n");
		} else {
			kill(_pid, SIGKILL);
		}
	}

	SendCommand("interrupt");
	FetchGdbResponse("(gdb)");
	SendCommand("quit");
	FetchGdbResponse("(gdb)");

	CalcAndPrintStatisticsAndExit();

	return 0;
}

void PerformStatsOnSingleThread(std::string threadName,
								const std::vector<StackFrame> &frames)
{
	std::map<std::string, size_t> samplesPerFunction;
	std::map<std::vector<size_t>, size_t> samplesPerUniqueStack;
	
	std::unordered_set<std::string> functionsAdded;
	functionsAdded.reserve(100);
	
	for (const StackFrame &frame : frames) {
		samplesPerUniqueStack[frame.stackFrameLines]++;
		functionsAdded.clear();
		
		for (size_t lineId : frame.stackFrameLines) {
			const StackFrameLine &line = individualStackFrameLines[lineId];
			if (functionsAdded.count(line.functionName) == 0) {
				functionsAdded.insert(line.functionName);
				samplesPerFunction[line.functionName] ++;
			}
		}
	}
	
	size_t totalSamples = frames.size();
	
	for (int i=0; i<5; ++i) {
	}
}

void CalcAndPrintStatisticsAndExit()
{
	printf("Collected %li total samples\n\n", totalCapturedFramesCount);
	
	for (const auto &it : capturedStackFramesPerThread) {
		PerformStatsOnSingleThread(it.first, it.second);
	}
	
	
	//
	// 	for (auto &s : capturedStackFrames) {
	// 		printf("\n`%s`\n\n", s.c_str());
	// 	}
	//
	// 	printf("Collected %li samples\n\n", rawSamples.size());
}

#endif

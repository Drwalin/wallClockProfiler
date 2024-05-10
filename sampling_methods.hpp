#pragma once

#ifndef WCPROF_SAMPLING_METHODS_HPP
#define WCPROF_SAMPLING_METHODS_HPP

#include "wcprof.cpp"

inline size_t GetStackFrameLineId(const std::string &line)
{
	auto it = mapIndividualStackFrameLinesToId.find(line);
	if (it != mapIndividualStackFrameLinesToId.end()) {
		return it->second;
	}

	auto pos = line.find("#");
	pos = line.find(" ", pos);
	auto lineBeginPos = line.find("0x", pos);
	const auto funcNamePos = line.find(" in ", lineBeginPos);
	if (funcNamePos == std::string::npos) {
		return -1;
	}

	auto p1 = line.find(" (...) ", funcNamePos);
	auto p2 = line.find(" at ", funcNamePos);

	const auto endFuncPos = std::min(p1, p2);
	if (endFuncPos == std::string::npos) {
		return -1;
	}

	StackFrameLine l;
	l.fullLine = line.substr(lineBeginPos);
	l.functionName = line.substr(funcNamePos, endFuncPos - funcNamePos);

	const size_t ret = individualStackFrameLines.size();
	individualStackFrameLines.push_back(std::move(l));
	mapIndividualStackFrameLinesToId[line] = ret;
	return ret;
}

inline std::vector<std::string_view> SplitGdbResponseIntoLines(const std::string &raw)
{
	std::vector<std::string_view> ret;
	auto pos = std::string::npos;
	pos = 0;
	auto lastPos = pos;
	while (true) {
		lastPos = pos;
		pos = raw.find("\n", lastPos);
		if (pos == std::string::npos || pos == lastPos) {
			return ret;
		}
		// 20 - arbitrary number, to ignore empty lines
		if (pos - lastPos < 20) {
			continue;
		}
		ret.push_back(std::string_view(raw.c_str()+lastPos+1, pos));
	}
	return ret;
}

inline void ParseSingleThreadStack(const std::string &threadName,
								   std::string_view *array, int count)
{
	StackFrame frame;
	frame.threadName = threadName;
	for (int i=0; i<count; ++i) {
		const std::string_view str = array[i];
		if (str[0] != '#') {
			continue;
		}
		auto p = str.find(" 0x");
		if (p == std::string::npos) {
			continue;
		}
		
		auto s = str.substr(p);
		std::string l(s.begin(), s.end());
		size_t lineId = GetStackFrameLineId(l);
		if (lineId == -1) {
			continue;
		}
		
		frame.stackFrameLines.push_back(lineId);
	}
	if (frame.stackFrameLines.size() > 0) {
		capturedStackFramesPerThread[threadName].push_back(std::move(frame));
		totalCapturedFramesCount++;
	}
}

inline void CollectSingleThreadStackFrame()
{
	// Fetch samples
	SendCommand("interrupt");

	SendCommand(
		"backtrace -frame-arguments none -frame-info location-and-address");

	SendCommand("c &");

	FetchGdbResponse("(gdb)"); // interrupt
	FetchGdbResponse("(gdb)"); // backtrace
	
	/*
	 * perform all stack frames parsing
	 */
	
	auto lines = SplitGdbResponseIntoLines(gdbResponse);
	ParseSingleThreadStack("main", lines.data(), lines.size());
	
	FetchGdbResponse("(gdb)"); // c &
}

inline void CollectAllThreadsStackFrames()
{
	// Fetch samples
	SendCommand("interrupt");

	SendCommand("thread apply all backtrace -frame-arguments none -frame-info "
				"location-and-address");

	SendCommand("c &");

	FetchGdbResponse("(gdb)"); // interrupt
	FetchGdbResponse("(gdb)"); // backtrace
	/*
	 * perform all stack frames parsing
	 */
	auto lines = SplitGdbResponseIntoLines(gdbResponse);
	
	for (int i=0; i<lines.size(); ++i) {
		if (lines[i].find("Thread ") == 0) {
			++i;
			int j = i;
			for (; j<lines.size(); ++j) {
				if (lines[j][0] != '#') {
					break;
				}
			}
			if (i < j) {
				std::string tn(lines[i].begin(), lines[i].end());
				
				auto p1 = tn.find("(LWP ");
				auto p2 = tn.find(")", p1);
				
				if (p1 != std::string::npos && p2 != std::string::npos) {
					tn = tn.substr(p1+1, p2-p1);
				}
				
				ParseSingleThreadStack(tn, lines.data()+i, j=i);
			}
		}
	}
	
	FetchGdbResponse("(gdb)"); // c &
}

inline void CollectRoundRobinThreadsStackFrames(int maxThreads)
{
	printf("Round robin is not implemented yet.\n");
}

#endif

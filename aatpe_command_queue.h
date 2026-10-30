#ifndef AATPE_COMMAND_QUEUE_H
#define AATPE_COMMAND_QUEUE_H

#include <string>

namespace aatpe_cmdq {

static constexpr int kMaxQueueDepth = 64;

struct EnqueueResult {
    bool ok = false;
    std::string command_id;
    int error_code = 0;
    std::string message;
};

EnqueueResult commandQueueEnqueue(const std::string& line);

bool tryPush(const std::string& line, EnqueueResult& out);

int commandQueueDrain();

std::string commandStatusJson(const std::string& id);

int commandQueueDepth();

bool commandLineIsReplaySafe(const std::string& line);

}

#endif

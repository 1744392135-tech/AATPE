#pragma once

namespace aatpe {

bool isFlutterEngineThread(const char* comm);

bool isRenderWhitelisted(const char* comm);

bool aatpeIsSurfaceFlingerThread(const char* comm);

bool isBlacklistedConsumerThread(const char* comm);

bool isGameEngineThread(const char* comm, const char* procComm);

}

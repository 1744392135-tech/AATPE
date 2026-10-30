#ifndef AATPE_WEBUI_HTTP_H
#define AATPE_WEBUI_HTTP_H

#include <string>
#include <cstdio>

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size() &&
            (unsigned char)s[i+1] == 0x80 &&
            ((unsigned char)s[i+2] == 0xA8 || (unsigned char)s[i+2] == 0xA9)) {
            out += (((unsigned char)s[i+2]) == 0xA8) ? "\\u2028" : "\\u2029";
            i += 3;
            continue;
        }
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
        i++;
    }
    return out;
}

namespace aatpe {
namespace webui_http {

bool startHttpServer();

void stopHttpServer();

int getHttpPort();

bool hasActiveClients();

void ensureDaemonToken();

std::string buildAggregateStateJson();

void auditSensitiveCommand(const std::string& cmd, const std::string& source,
                           bool ok, const std::string& reason);

}
}

#endif

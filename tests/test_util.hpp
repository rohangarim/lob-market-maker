#pragma once

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace lob_test {

// mkstemp-based temp file path (std::tmpnam is deprecated and racy).
inline std::string unique_temp_path() {
    std::string tmpl = "/tmp/lob_test_XXXXXX";
    int fd = mkstemp(tmpl.data());
    if (fd != -1) close(fd);
    return tmpl;
}

}  // namespace lob_test

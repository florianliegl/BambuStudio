#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace Slic3r::ExtendedAMSLite {

// Bambu's public import-file URL scheme accepts a sliced file, not print commands.
// Encode UTF-8 bytes, including query delimiters, so paths stay one parameter.
inline std::string connect_uri_component(const std::string& value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            result += static_cast<char>(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

inline std::string connect_import_uri(const std::string& absolute_path, const std::string& name)
{
    if (absolute_path.empty() || name.empty() || absolute_path.find('\0') != std::string::npos || name.find('\0') != std::string::npos)
        throw std::invalid_argument("Bambu Connect needs a file path and name.");
    return "bambu-connect://import-file?path=" + connect_uri_component(absolute_path) +
        "&name=" + connect_uri_component(name) + "&version=1.0.0";
}

inline bool connect_job_supported(const std::string& printer_model, std::size_t used_filaments)
{
    // Submission is separate from swap execution. An A1 job with more than
    // four identities needs Studio's fixed mapping and native LAN submission.
    return used_filaments > 0 &&
        ((printer_model != "A1" && printer_model != "A1mini" &&
          printer_model != "Bambu Lab A1" && printer_model != "Bambu Lab A1 mini") || used_filaments <= 4);
}

} // namespace Slic3r::ExtendedAMSLite

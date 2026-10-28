#include "Planner.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    using namespace Slic3r::ExtendedAMSLite;
    const std::vector<Filament> filaments = {
        {"Black", "PLA", "#202020"}, {"Blue", "PLA", "#2463EB"},
        {"Red", "PLA", "#E34B4B"}, {"White", "PLA", "#F5F5F5"},
        {"Green", "PLA", "#22A86A"}, {"Yellow", "PLA", "#F2C438"},
        {"Purple", "PLA", "#9755CE"}, {"Orange", "PLA", "#ED8736"}};
    // Synthetic selection trace, not a physically validated print.
    const std::vector<int> ids = {0, 1, 2, 3, 4, 0, 5, 1, 6, 0, 7, 1, 2, 3, 4, 5, 6, 7};
    std::vector<Request> requests;
    for (std::size_t i = 0; i < ids.size(); ++i)
        requests.push_back({ids[i], static_cast<unsigned int>(i < 10 ? 42 : 43), i * 100, static_cast<unsigned int>(i * 120 + 1)});
    const auto result = plan(requests, filaments.size(), std::vector<Slot>(4));
    if (argc > 1 && std::string(argv[1]) == "--html")
        std::cout << format_schedule_html(filaments, requests, result);
    else
        std::cout << format_schedule(filaments, requests, result);
}

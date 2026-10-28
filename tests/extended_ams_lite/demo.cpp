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
    std::vector<Request> requests;
    for (unsigned layer = 1; layer <= 4; ++layer)
        for (int id = 0; id < 8; ++id)
            requests.push_back({id, layer, requests.size() * 100, static_cast<unsigned>(requests.size() * 120 + 1)});
    const auto result = plan_batches(requests, filaments.size(), std::vector<Slot>(4), {}, true);
    if (argc > 1 && std::string(argv[1]) == "--html")
        std::cout << format_batch_schedule_html(filaments, result);
    else
        std::cout << format_batch_schedule(filaments, result);
}

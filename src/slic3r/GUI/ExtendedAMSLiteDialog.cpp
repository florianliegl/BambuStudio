#include "ExtendedAMSLiteDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <mutex>
#include <stdexcept>

namespace Slic3r { namespace GUI {
namespace AMS = ExtendedAMSLite;

ExtendedAMSLiteDialog::ExtendedAMSLiteDialog(wxWindow* parent, const GCodeProcessorResult& result)
    : DPIDialog(parent, wxID_ANY, _L("Extended AMS Lite Mode - Dry run"), wxDefaultPosition,
                wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    // Copy a coherent slice snapshot. No callbacks retain references to a plate
    // or background-processing result that could change while this dialog lives.
    {
        std::lock_guard<std::mutex> guard(result.result_mutex);
        if (!result.used_mixed_filaments.empty() || result.extruder_types.size() > 1)
            throw std::invalid_argument("The AMS Lite planner currently supports single-extruder prints without mixed filaments. Select an A1 profile and re-slice.");
        m_requests = AMS::collect_requests(result.moves, EMoveType::Tool_change, EMoveType::Extrude);
        for (std::size_t i = 0; i < result.filaments_count; ++i) {
            AMS::Filament filament;
            filament.name = i < result.settings_ids.filament.size() ? result.settings_ids.filament[i] : "Virtual filament " + std::to_string(i + 1);
            filament.material = i < result.filament_types.size() ? result.filament_types[i] : "Unknown material";
            filament.color = i < result.extruder_colors.size() ? result.extruder_colors[i] : "#808080";
            m_filaments.push_back(std::move(filament));
        }
    }

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* warning = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(AMS::execution_blocker()));
    warning->Wrap(82 * em_unit());
    root->Add(warning, 0, wxEXPAND | wxALL, em_unit());

    auto* count_row = new wxBoxSizer(wxHORIZONTAL);
    count_row->Add(new wxStaticText(this, wxID_ANY, _L("Physical slots (standard AMS Lite: 4)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em_unit());
    m_slot_count = new wxSpinCtrl(this, wxID_ANY, "4", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 32, 4);
    count_row->Add(m_slot_count, 0);
    auto* calculate_button = new wxButton(this, wxID_ANY, _L("Calculate swap schedule"));
    count_row->Add(calculate_button, 0, wxLEFT, em_unit());
    root->Add(count_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, em_unit());

    root->Add(new wxStaticText(this, wxID_ANY, _L("Initial slot overrides: Auto chooses a filament; Lock keeps that filament in its slot.")), 0, wxLEFT | wxRIGHT, em_unit());
    m_slot_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 13 * em_unit()));
    m_slot_panel->SetScrollRate(0, em_unit());
    m_slot_sizer = new wxFlexGridSizer(3, em_unit() / 2, em_unit());
    m_slot_sizer->AddGrowableCol(1);
    m_slot_panel->SetSizer(m_slot_sizer);
    root->Add(m_slot_panel, 0, wxEXPAND | wxALL, em_unit());
    rebuild_slots();

    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, em_unit());
    m_initial_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxSize(-1, 7 * em_unit()), wxTE_MULTILINE | wxTE_READONLY);
    root->Add(m_initial_text, 0, wxEXPAND | wxALL, em_unit());
    m_schedule = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    const wxString columns[] = {_L("Planned stop"), _L("Layer"), _L("Slot"), _L("Remove"), _L("Insert / next required"), _L("Move"), _L("G-code line ID")};
    const int widths[] = {9, 8, 5, 25, 25, 8, 12};
    for (int i = 0; i < 7; ++i)
        m_schedule->InsertColumn(i, columns[i], wxLIST_FORMAT_LEFT, widths[i] * em_unit());
    root->Add(m_schedule, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_export = new wxButton(this, wxID_ANY, _L("Export schedule..."));
    buttons->Add(m_export, 0);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0);
    root->Add(buttons, 0, wxEXPAND | wxALL, em_unit());
    SetSizer(root);
    SetSize(wxSize(100 * em_unit(), 65 * em_unit()));
    SetMinSize(wxSize(70 * em_unit(), 50 * em_unit()));
    wxGetApp().UpdateDarkUI(this);
    CenterOnParent();

    m_slot_count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { rebuild_slots(); invalidate(); });
    m_slot_count->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { rebuild_slots(); invalidate(); });
    calculate_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { calculate(); });
    m_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_schedule(); });
    calculate();
}

wxString ExtendedAMSLiteDialog::filament_label(int id) const
{
    if (id < 0)
        return _L("Empty");
    const auto& filament = m_filaments.at(std::size_t(id));
    return wxString::FromUTF8("V" + std::to_string(id + 1) + " " + filament.name + " [" + filament.material + ", " + filament.color + "]");
}

void ExtendedAMSLiteDialog::rebuild_slots()
{
    const int count = m_slot_count->GetValue();
    if (count == static_cast<int>(m_initial.size()))
        return;
    m_slot_sizer->Clear(true);
    m_initial.clear();
    m_locked.clear();
    for (int s = 0; s < count; ++s) {
        m_slot_sizer->Add(new wxStaticText(m_slot_panel, wxID_ANY, wxString::Format(_L("Slot %d"), s + 1)), 0, wxALIGN_CENTER_VERTICAL);
        auto* choice = new wxChoice(m_slot_panel, wxID_ANY);
        choice->Append(_L("Auto"));
        for (std::size_t i = 0; i < m_filaments.size(); ++i)
            choice->Append(filament_label(static_cast<int>(i)));
        choice->SetSelection(0);
        m_slot_sizer->Add(choice, 1, wxEXPAND);
        m_initial.push_back(choice);
        auto* locked = new wxCheckBox(m_slot_panel, wxID_ANY, _L("Lock"));
        m_slot_sizer->Add(locked, 0, wxALIGN_CENTER_VERTICAL);
        m_locked.push_back(locked);
        choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { invalidate(); });
        locked->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { invalidate(); });
    }
    m_slot_panel->FitInside();
    m_slot_panel->Layout();
}

void ExtendedAMSLiteDialog::invalidate()
{
    m_export_text.clear();
    m_export_html.clear();
    m_export->Disable();
    m_schedule->DeleteAllItems();
    m_initial_text->Clear();
    m_summary->SetLabel(_L("Configuration changed. Calculate the swap schedule again."));
}

void ExtendedAMSLiteDialog::calculate()
{
    invalidate();
    try {
        std::vector<AMS::Slot> slots;
        for (std::size_t s = 0; s < m_initial.size(); ++s)
            slots.push_back({m_initial[s]->GetSelection() - 1, m_locked[s]->GetValue()});
        const AMS::Plan result = AMS::plan(m_requests, m_filaments.size(), slots);
        m_export_text = AMS::format_schedule(m_filaments, m_requests, result);
        m_export_html = AMS::format_schedule_html(m_filaments, m_requests, result);
        wxString initial = _L("Initial loading (simulation):\n");
        for (std::size_t s = 0; s < result.initial_slots.size(); ++s)
            initial += wxString::Format(_L("Slot %d: "), static_cast<int>(s + 1)) + filament_label(result.initial_slots[s]) + "\n";
        m_initial_text->SetValue(initial);
        m_summary->SetLabel(wxString::Format(_L("Dry run: %d filament runs, %d planned stops. No G-code changes."),
                                           static_cast<int>(m_requests.size()), static_cast<int>(result.stops.size())));
        for (std::size_t i = 0; i < result.stops.size(); ++i) {
            const auto& stop = result.stops[i];
            const auto& request = m_requests[stop.before_request];
            for (const auto& replacement : stop.replacements) {
                const long row = m_schedule->InsertItem(m_schedule->GetItemCount(), wxString::Format("%d / %d", static_cast<int>(i + 1), static_cast<int>(result.stops.size())));
                m_schedule->SetItem(row, 1, request.layer ? wxString::Format("%u", request.layer) : _L("Startup/?"));
                m_schedule->SetItem(row, 2, wxString::Format("%d", static_cast<int>(replacement.slot + 1)));
                m_schedule->SetItem(row, 3, filament_label(replacement.remove));
                m_schedule->SetItem(row, 4, filament_label(replacement.insert));
                m_schedule->SetItem(row, 5, wxString::Format("%llu", static_cast<unsigned long long>(request.move)));
                m_schedule->SetItem(row, 6, wxString::Format("%u", request.gcode_line));
                const wxColour color(wxString::FromUTF8(m_filaments[replacement.insert].color));
                if (color.IsOk()) {
                    m_schedule->SetItemBackgroundColour(row, color);
                    m_schedule->SetItemTextColour(row, (299 * color.Red() + 587 * color.Green() + 114 * color.Blue() > 128000) ? *wxBLACK : *wxWHITE);
                }
            }
        }
        m_export->Enable();
    } catch (const std::exception& error) {
        m_summary->SetLabel(wxString::FromUTF8(error.what()));
        m_summary->Wrap(85 * em_unit());
    }
    Layout();
}

void ExtendedAMSLiteDialog::export_schedule()
{
    if (m_export_text.empty())
        return;
    wxFileDialog dialog(this, _L("Export dry-run swap schedule"), wxEmptyString, "extended-ams-lite-schedule",
                        _L("Text files (*.txt)|*.txt|HTML files (*.html)|*.html"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK)
        return;
    wxFile file(dialog.GetPath(), wxFile::write);
    const auto& contents = dialog.GetFilterIndex() == 1 ? m_export_html : m_export_text;
    if (!file.IsOpened() || !file.Write(wxString::FromUTF8(contents), wxConvUTF8) || !file.Close())
        wxMessageBox(_L("Could not write the swap schedule."), _L("Export failed"), wxOK | wxICON_ERROR, this);
}

void ExtendedAMSLiteDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    SetMinSize(wxSize(70 * em_unit(), 50 * em_unit()));
    SetSize(suggested_rect);
    Layout();
    Refresh();
}

}} // namespace Slic3r::GUI

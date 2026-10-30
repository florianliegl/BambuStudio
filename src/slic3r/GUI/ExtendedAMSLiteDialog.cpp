#include "ExtendedAMSLiteDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
namespace Slic3r { namespace GUI {
namespace AMS = ExtendedAMSLite;
ExtendedAMSLiteDialog::ExtendedAMSLiteDialog(wxWindow* parent, const GCodeProcessorResult& result,
    bool can_reorder, const std::vector<std::string>& names, const AMS::ExecutionSpec* ready)
    : DPIDialog(parent, wxID_ANY, _L("Extended AMS Lite"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_can_reorder(can_reorder && !ready), m_ready(ready != nullptr)
{
    if (ready) {
        m_filaments = ready->filaments;
        m_requests = ready->plan.requests;
        m_plan = ready->plan;
        m_magnet_pauses = ready->magnet_pauses;
    } else {
        std::lock_guard<std::mutex> guard(result.result_mutex);
        if (!result.used_mixed_filaments.empty() || result.extruder_types.size() > 1)
            throw std::invalid_argument("Batch planning supports single-extruder prints without mixed filaments. Select an A1 profile and re-slice.");
        m_requests = AMS::collect_requests(result.moves, EMoveType::Tool_change, EMoveType::Extrude, true);
        if (!result.filament_change_sequence.empty())
            m_requests = AMS::normalize_execution_requests(std::move(m_requests), int(result.filament_change_sequence.front()));
        for (std::size_t i = 0; i < result.filaments_count; ++i)
            m_filaments.push_back({i < names.size() ? names[i] : (i < result.settings_ids.filament.size() ? result.settings_ids.filament[i] : "Filament " + std::to_string(i + 1)),
                i < result.filament_types.size() ? result.filament_types[i] : "Unknown material",
                i < result.extruder_colors.size() ? result.extruder_colors[i] : ""});
    }
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* heading = new wxStaticText(this, wxID_ANY, _L("Fewer visits. Swap a whole batch together."));
    heading->SetFont(heading->GetFont().Bold().Larger());
    root->Add(heading, 0, wxALL, em_unit());
    auto* status = new wxStaticText(this, wxID_ANY, ready ?
        _L("Print prepared. At each swap pause the active filament is unloaded. Replace the spools shown below, feed each to the AMS inlet, then press Resume on the printer. Keep all eight colors PLA. Choose the visit manually; printer pauses do not identify a visit.") :
        _L("Prepare an A1 PLA print with one unload-and-swap pause per visit. The normal filament-change and purge commands reload the next color. Print directly from this Studio using LAN-only Developer Mode."));
    if (ready && !ready->magnet_pauses.empty()) {
        wxString text = status->GetLabel() + "\n\n" + _L("Separate magnet insertion pauses at Z:");
        for (double z : ready->magnet_pauses) text += wxString::Format(" %.2f mm", z);
        status->SetLabel(text + ". " + _L("At these pauses insert magnets and keep all spools loaded. They do not advance the spool-swap visits."));
    }
    status->Wrap(85 * em_unit());
    root->Add(status, 0, wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    m_optimize = new wxCheckBox(this, wxID_ANY, _L("Optimize layers: keep the loaded batch for the next layer"));
    m_optimize->SetValue(m_can_reorder);
    m_optimize->Enable(m_can_reorder);
    m_optimize->SetToolTip(_L("Reordering requires an editable project printing by layer. Imported G-code can only be analyzed."));
    root->Add(m_optimize, 0, wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_summary->SetFont(m_summary->GetFont().Bold());
    root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    root->Add(new wxStaticText(this, wxID_ANY, _L("Load these first")), 0, wxLEFT | wxRIGHT, em_unit());
    auto* loading = new wxBoxSizer(wxHORIZONTAL);
    for (std::size_t s = 0; s < 4; ++s) m_loading[s] = make_card(loading, wxString::Format(_L("Slot %d"), int(s + 1)));
    root->Add(loading, 0, wxEXPAND | wxALL, em_unit());
    m_visits = new wxChoice(this, wxID_ANY);
    root->Add(m_visits, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    auto* changes = new wxBoxSizer(wxHORIZONTAL);
    for (std::size_t s = 0; s < 4; ++s) m_changes[s] = make_card(changes, wxString::Format(_L("Slot %d"), int(s + 1)));
    root->Add(changes, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    m_detail = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_detail, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    auto* advanced = new wxCollapsiblePane(this, wxID_ANY, _L("First-layer order and slot settings"), wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    auto* pane = advanced->GetPane();
    auto* settings = new wxBoxSizer(wxVERTICAL);
    m_manual = new wxCheckBox(pane, wxID_ANY, _L("Use my first-layer order (select a color, then move it up or down)"));
    m_manual->Enable(m_can_reorder);
    settings->Add(m_manual, 0, wxALL, em_unit() / 2);
    auto* order_row = new wxBoxSizer(wxHORIZONTAL);
    m_order = new wxListBox(pane, wxID_ANY, wxDefaultPosition, wxSize(-1, 8 * em_unit()));
    const auto first = std::find_if(m_requests.begin(), m_requests.end(), [](const AMS::Request& r) { return r.layer > 0; });
    if (first != m_requests.end()) for (const auto& r : m_requests) {
        if (r.layer != first->layer || std::find(m_order_ids.begin(), m_order_ids.end(), r.filament) != m_order_ids.end()) continue;
        m_order_ids.push_back(r.filament);
        m_order->Append(filament_label(r.filament));
    }
    m_order->Enable(false);
    order_row->Add(m_order, 1, wxEXPAND);
    auto* arrows = new wxBoxSizer(wxVERTICAL);
    auto* up = new wxButton(pane, wxID_ANY, _L("Up"));
    auto* down = new wxButton(pane, wxID_ANY, _L("Down"));
    arrows->Add(up, 0, wxALL, em_unit() / 2);
    arrows->Add(down, 0, wxALL, em_unit() / 2);
    order_row->Add(arrows, 0);
    settings->Add(order_row, 0, wxEXPAND | wxALL, em_unit() / 2);
    settings->Add(new wxStaticText(pane, wxID_ANY, _L("Auto chooses the initial color. Lock keeps that spool loaded throughout the job.")), 0, wxALL, em_unit() / 2);
    auto* grid = new wxFlexGridSizer(3, em_unit() / 2, em_unit());
    grid->AddGrowableCol(1);
    for (std::size_t s = 0; s < 4; ++s) {
        grid->Add(new wxStaticText(pane, wxID_ANY, wxString::Format(_L("Slot %d"), int(s + 1))), 0, wxALIGN_CENTER_VERTICAL);
        m_initial[s] = new wxChoice(pane, wxID_ANY);
        m_initial[s]->Append(_L("Auto"));
        for (std::size_t id = 0; id < m_filaments.size(); ++id) m_initial[s]->Append(filament_label(int(id)));
        m_initial[s]->SetSelection(0);
        grid->Add(m_initial[s], 1, wxEXPAND);
        m_locked[s] = new wxCheckBox(pane, wxID_ANY, _L("Lock"));
        if (ready) {
            m_initial[s]->SetSelection(ready->slots[s].filament + 1);
            m_locked[s]->SetValue(ready->slots[s].locked);
            m_initial[s]->Disable(); m_locked[s]->Disable();
        }
        grid->Add(m_locked[s], 0, wxALIGN_CENTER_VERTICAL);
        m_initial[s]->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { calculate(); });
        m_locked[s]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { calculate(); });
    }
    settings->Add(grid, 0, wxEXPAND | wxALL, em_unit() / 2);
    pane->SetSizer(settings);
    root->Add(advanced, 0, wxEXPAND | wxLEFT | wxRIGHT, em_unit());
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_export = new wxButton(this, wxID_ANY, _L("Export plan..."));
    m_apply = new wxButton(this, wxID_APPLY, _L("Apply order && slice"));
    m_prepare = new wxButton(this, wxID_OK, _L("Prepare print"));
    buttons->Add(m_export, 0);
    buttons->AddStretchSpacer();
    buttons->Add(m_apply, 0, wxRIGHT, em_unit());
    buttons->Add(m_prepare, 0, wxRIGHT, em_unit());
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0);
    root->Add(buttons, 0, wxEXPAND | wxALL, em_unit());
    SetSizer(root);
    SetMinSize(wxSize(88 * em_unit(), 57 * em_unit()));
    SetSize(wxSize(94 * em_unit(), 60 * em_unit()));
    wxGetApp().UpdateDarkUI(this);
    CenterOnParent();
    advanced->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) { GetSizer()->Fit(this); Layout(); });
    m_optimize->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (!m_optimize->GetValue()) { m_manual->SetValue(false); m_order->Disable(); }
        calculate();
    });
    m_manual->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { if (m_manual->GetValue()) m_optimize->SetValue(true); m_order->Enable(m_manual->GetValue()); calculate(); });
    up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { move_color(-1); });
    down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { move_color(1); });
    m_visits->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { show_visit(); });
    m_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_schedule(); });
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_valid && m_plan.reordered && m_can_reorder) EndModal(wxID_APPLY); });
    m_prepare->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_valid && m_can_reorder) EndModal(wxID_OK); });
    if (ready) { m_filaments = ready->filaments; m_requests = ready->plan.requests; m_plan = ready->plan; }
    calculate();
}
ExtendedAMSLiteDialog::Card ExtendedAMSLiteDialog::make_card(wxSizer* row, const wxString& title)
{
    auto* panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* top = new wxBoxSizer(wxHORIZONTAL);
    auto* color = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(2 * em_unit(), 2 * em_unit()), wxBORDER_SIMPLE);
    top->Add(color, 0, wxRIGHT, em_unit() / 2);
    auto* heading = new wxStaticText(panel, wxID_ANY, title);
    heading->SetFont(heading->GetFont().Bold());
    top->Add(heading, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(top, 0, wxALL, em_unit());
    auto* text = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(18 * em_unit(), 9 * em_unit()));
    box->Add(text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
    panel->SetSizer(box);
    row->Add(panel, 1, wxEXPAND | wxRIGHT, em_unit() / 2);
    return {color, text};
}
wxString ExtendedAMSLiteDialog::filament_label(int id) const
{
    if (id < 0) return _L("Empty");
    const auto& f = m_filaments.at(std::size_t(id));
    return wxString::FromUTF8("V" + std::to_string(id + 1) + " " + AMS::color_name(f.color) + " / " + f.material + " / " + f.color);
}
void ExtendedAMSLiteDialog::set_card(Card& card, int id, const wxString& text)
{
    card.text->SetLabel(text);
    card.text->Wrap(18 * em_unit());
    wxColour color = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    if (id >= 0 && AMS::color_name(m_filaments.at(std::size_t(id)).color) != "Unknown color")
        color = wxColour(wxString::FromUTF8(m_filaments.at(std::size_t(id)).color.substr(0, 7)));
    card.color->SetBackgroundColour(color);
    card.color->SetToolTip(id >= 0 ? wxString::FromUTF8(m_filaments.at(std::size_t(id)).name) : wxEmptyString);
    card.color->Refresh();
}
void ExtendedAMSLiteDialog::calculate()
{
    m_valid = false;
    m_export->Disable();
    m_apply->Disable();
    m_prepare->Disable();
    m_visits->Clear();
    for (auto& card : m_loading) set_card(card, -1, _L("Recalculating..."));
    for (auto& card : m_changes) set_card(card, -1, wxEmptyString);
    try {
        std::vector<AMS::Slot> slots;
        for (std::size_t s = 0; s < 4; ++s) slots.push_back({m_initial[s]->GetSelection() - 1, m_locked[s]->GetValue()});
        if (!m_ready) m_plan = AMS::plan_batches(m_requests, m_filaments.size(), slots, m_manual->GetValue() ? m_order_ids : std::vector<int>{}, m_optimize->GetValue());
        for (std::size_t s = 0; s < 4; ++s) set_card(m_loading[s], m_plan.schedule.initial_slots[s], filament_label(m_plan.schedule.initial_slots[s]));
        m_summary->SetLabel(wxString::Format(_L("%d visits  /  %d spool replacements  /  %d color changes"), int(m_plan.schedule.stops.size()), int(m_plan.spool_replacements), int(m_plan.color_changes)));
        for (std::size_t i = 0; i < m_plan.schedule.stops.size(); ++i) {
            const auto& stop = m_plan.schedule.stops[i];
            const auto& r = m_plan.requests[stop.before_request];
            m_visits->Append(wxString::Format(_L("Visit %d of %d - Layer %u - swap %d spools together"), int(i + 1), int(m_plan.schedule.stops.size()), r.layer, int(stop.replacements.size())));
        }
        if (m_plan.schedule.stops.empty()) m_visits->Append(_L("No spool swaps required"));
        m_visits->SetSelection(0);
        m_valid = true;
        m_export->Enable();
        m_apply->Enable(m_plan.reordered && m_can_reorder);
        m_prepare->Enable(m_can_reorder && !m_plan.schedule.stops.empty() &&
            std::all_of(m_filaments.begin(), m_filaments.end(), [](const AMS::Filament& f) { return f.material == "PLA"; }));
        if (m_ready) { m_apply->Hide(); m_prepare->Hide(); }
        show_visit();
    } catch (const std::exception& error) {
        m_summary->SetLabel(wxString::FromUTF8(error.what()));
        m_summary->Wrap(85 * em_unit());
        m_detail->SetLabel(_L("Change the slot settings and try again."));
    }
    Layout();
}
void ExtendedAMSLiteDialog::show_visit()
{
    if (!m_valid) return;
    for (auto& card : m_changes) set_card(card, -1, _L("Keep loaded"));
    if (m_plan.schedule.stops.empty()) { m_detail->SetLabel(_L("All required colors fit in the four slots.")); return; }
    const auto& stop = m_plan.schedule.stops.at(std::size_t(m_visits->GetSelection()));
    const auto& r = m_plan.requests[stop.before_request];
    for (const auto& c : stop.replacements)
        set_card(m_changes[c.slot], c.insert, _L("Remove: ") + filament_label(c.remove) + "\n" + _L("Insert: ") + filament_label(c.insert));
    wxString detail = _L("Next: ") + filament_label(r.filament);
    detail += m_ready ? _L("\nOne pause for this visit. Replace only the spools marked Insert. Resume after every replacement is complete.") :
        _L("\nChoose Prepare print to regenerate this plate with the required unload-and-swap pauses.");
    m_detail->SetLabel(detail);
    m_detail->Wrap(85 * em_unit());
    Layout();
}
void ExtendedAMSLiteDialog::move_color(int direction)
{
    if (!m_manual->GetValue()) return;
    const int selected = m_order->GetSelection(), next = selected + direction;
    if (selected < 0 || next < 0 || next >= int(m_order_ids.size())) return;
    std::swap(m_order_ids[selected], m_order_ids[next]);
    m_order->SetString(selected, filament_label(m_order_ids[selected]));
    m_order->SetString(next, filament_label(m_order_ids[next]));
    m_order->SetSelection(next);
    calculate();
}
void ExtendedAMSLiteDialog::export_schedule()
{
    if (!m_valid) return;
    wxFileDialog dialog(this, _L("Export batch plan"), wxEmptyString, "extended-ams-lite-batches", _L("HTML files (*.html)|*.html|Text files (*.txt)|*.txt"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) return;
    const std::string content = m_ready ? AMS::format_execution_guide(proposal(), dialog.GetFilterIndex() == 0) :
        (dialog.GetFilterIndex() == 0 ? AMS::format_batch_schedule_html(m_filaments, m_plan) : AMS::format_batch_schedule(m_filaments, m_plan));
    wxFile file(dialog.GetPath(), wxFile::write);
    if (!file.IsOpened() || !file.Write(wxString::FromUTF8(content), wxConvUTF8) || !file.Close())
        wxMessageBox(_L("Could not write the batch plan."), _L("Export failed"), wxOK | wxICON_ERROR, this);
}
AMS::ExecutionSpec ExtendedAMSLiteDialog::proposal() const
{
    AMS::ExecutionSpec spec;
    spec.filaments = m_filaments;
    spec.plan = m_plan;
    spec.magnet_pauses = m_magnet_pauses;
    for (std::size_t s = 0; s < 4; ++s) spec.slots.push_back({m_initial[s]->GetSelection() - 1, m_locked[s]->GetValue()});
    return spec;
}
void ExtendedAMSLiteDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    SetMinSize(wxSize(88 * em_unit(), 57 * em_unit()));
    SetSize(suggested_rect);
    Layout();
    Refresh();
}
int show_extended_ams_lite_dialog(wxWindow* parent, const GCodeProcessorResult& result, bool can_reorder,
    const std::vector<std::string>& names, const AMS::ExecutionSpec* ready,
    std::vector<AMS::LayerOrder>* orders, AMS::ExecutionSpec* proposal)
{
    ExtendedAMSLiteDialog dialog(parent, result, can_reorder, names, ready);
    const int action = dialog.ShowModal();
    if (action == wxID_APPLY || action == wxID_OK) {
        if (orders) *orders = dialog.layer_orders();
        if (proposal) *proposal = dialog.proposal();
    }
    return action;
}
namespace {
class InitialLoadingDialog : public DPIDialog {
public:
    InitialLoadingDialog(wxWindow* parent, const AMS::ExecutionSpec& spec)
        : DPIDialog(parent, wxID_ANY, _L("Check initial AMS Lite loading"), wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        AMS::verify_execution(spec);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* heading = new wxStaticText(this, wxID_ANY, _L("Load these colors before printing"));
        heading->SetFont(heading->GetFont().Bold().Larger());
        root->Add(heading, 0, wxALL, em_unit());
        root->Add(new wxStaticText(this, wxID_ANY, _L("Match each spool to its numbered slot.")), 0, wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        std::array<wxPanel*, 4> swatches;
        std::array<wxColour, 4> colors;
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const int id = spec.plan.schedule.initial_slots[slot];
            const AMS::Filament* filament = id < 0 ? nullptr : &spec.filaments.at(std::size_t(id));
            const auto name = filament ? AMS::color_name(filament->color) : std::string("Not used at start");
            auto* panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
            auto* card = new wxBoxSizer(wxVERTICAL);
            auto* title = new wxStaticText(panel, wxID_ANY, wxString::Format(_L("Slot %u (A%u)"), unsigned(slot + 1), unsigned(slot + 1)));
            title->SetFont(title->GetFont().Bold());
            card->Add(title, 0, wxALL, em_unit());
            swatches[slot] = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(17 * em_unit(), 7 * em_unit()), wxBORDER_SIMPLE);
            colors[slot] = filament && name != "Unknown color" ? wxColour(wxString::FromUTF8(filament->color.substr(0, 7))) : wxColour(180, 180, 180);
            card->Add(swatches[slot], 0, wxEXPAND | wxLEFT | wxRIGHT, em_unit());
            auto* color_name = new wxStaticText(panel, wxID_ANY, wxString::FromUTF8(name));
            color_name->SetFont(color_name->GetFont().Bold().Larger());
            card->Add(color_name, 0, wxALL, em_unit());
            if (filament) {
                auto* identity = new wxStaticText(panel, wxID_ANY, wxString::FromUTF8("V" + std::to_string(id + 1) + " / " + filament->material + "\n" + filament->color));
                card->Add(identity, 0, wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
                auto* profile = new wxStaticText(panel, wxID_ANY, wxString::FromUTF8(filament->name));
                profile->SetFont(profile->GetFont().Smaller());
                profile->Wrap(17 * em_unit());
                profile->SetToolTip(wxString::FromUTF8(filament->name));
                card->Add(profile, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
            }
            panel->SetSizer(card); row->Add(panel, 1, wxEXPAND | wxRIGHT, em_unit() / 2);
        }
        root->Add(row, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* note = new wxStaticText(this, wxID_ANY, _L("Names are based on your project colors. Use the swap guide for later spool replacements."));
        note->Wrap(80 * em_unit());
        root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* buttons = new wxBoxSizer(wxHORIZONTAL); buttons->AddStretchSpacer();
        auto* back = new wxButton(this, wxID_CANCEL, _L("Back"));
        auto* send = new wxButton(this, wxID_OK, _L("Confirm loading and send"));
        buttons->Add(back, 0, wxRIGHT, em_unit()); buttons->Add(send);
        root->Add(buttons, 0, wxEXPAND | wxALL, em_unit());
        SetSizerAndFit(root); SetMinSize(GetSize());
        wxGetApp().UpdateDarkUI(this);
        for (std::size_t slot = 0; slot < 4; ++slot) swatches[slot]->SetBackgroundColour(colors[slot]);
        back->SetDefault(); back->SetFocus(); SetEscapeId(wxID_CANCEL); CenterOnParent();
    }
protected:
    void on_dpi_changed(const wxRect&) override { Layout(); Refresh(); }
};
}
bool confirm_extended_ams_initial_loading(wxWindow* parent, const AMS::ExecutionSpec& spec)
{
    InitialLoadingDialog dialog(parent, spec);
    return dialog.ShowModal() == wxID_OK;
}
}}

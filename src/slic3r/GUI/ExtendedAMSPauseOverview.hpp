#pragma once
// Private implementation included by StatusPanel.cpp. Keep the public panel ABI unchanged.
#include "libslic3r/ExtendedAMSLite/PauseTimeline.hpp"
#include <boost/filesystem/fstream.hpp>
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>

namespace Slic3r::GUI {
namespace {
namespace PauseAMS = ExtendedAMSLite;
constexpr const char* pause_overview_name = "extended_ams_pause_overview";

class ExtendedAMSPauseOverview final : public wxPanel {
    wxStaticText *m_title, *m_detail;
    wxButton *m_show, *m_adjust;
    std::optional<PauseAMS::ExecutionSpec> m_spec;
    PauseAMS::PauseTimeline m_timeline;
    PauseAMS::PauseProgress m_progress;
    PauseAMS::PauseObservation m_state;
    std::string m_target, m_key, m_id, m_run;
    boost::filesystem::path m_folder;
    bool m_saved_progress = false;
    bool m_was_online = true;
    wxString m_render_title, m_render_detail;

    void relayout()
    {
        Layout(); GetParent()->Layout();
        if (auto* page = dynamic_cast<wxScrolledWindow*>(GetParent()->GetParent())) {
            page->Layout(); page->FitInside();
        }
    }
    void set_visible(bool visible) { if (Show(visible)) relayout(); }

    static std::string read_file(const boost::filesystem::path& path, std::size_t limit, bool optional = false)
    {
        if (optional && !boost::filesystem::exists(path)) return {};
        if (boost::filesystem::file_size(path) > limit) throw std::runtime_error("Saved pause overview is too large.");
        boost::filesystem::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Could not read the saved pause overview.");
        std::string text{std::istreambuf_iterator<char>(in), {}};
        if (in.bad()) throw std::runtime_error("Could not read the saved pause overview.");
        return text;
    }
    static PauseAMS::PauseObservation observation(MachineObject* obj)
    {
        PauseAMS::PauseObservation state;
        if (obj) {
            state.printer = obj->get_dev_id(); state.job_name = obj->subtask_name;
            state.connected = obj->is_connected(); state.ready = obj->is_info_ready();
            state.status = obj->print_status; state.layer = unsigned(std::max(0, obj->curr_layer));
            state.error = obj->print_error ? obj->print_error : obj->mc_print_error_code;
            state.remaining_seconds = obj->mc_left_time;
        }
        return state;
    }
    static PauseAMS::PauseObservation current_observation()
    {
        auto* manager = wxGetApp().getDeviceManager();
        return observation(manager ? manager->get_selected_machine() : nullptr);
    }
    wxString event_label(const PauseAMS::PauseEvent& event) const
    {
        wxString text = event.kind == PauseAMS::PauseKind::SpoolSwap ?
            wxString::Format("Swap visit %d of %d", int(event.visit + 1), int(m_spec->plan.schedule.stops.size())) :
            wxString::Format("Insert magnets at Z %.2f mm", event.z);
        if (event.layer) text += wxString::Format(" | Layer %u", event.layer);
        return text;
    }
    void labels(const wxString& title, const wxString& detail)
    {
        if (m_render_title == title && m_render_detail == detail) return;
        m_render_title = title; m_render_detail = detail;
        m_title->SetLabel(title); m_detail->SetLabel(detail); m_detail->Wrap(FromDIP(520));
        relayout();
    }
    bool save_progress()
    {
        // Older submissions have no unique run token: keep their progress session-local.
        if (m_run.empty()) return true;
        try {
            if (read_file(m_folder / (m_id + ".run"), 512) != m_run) return false;
            const auto temp = m_folder / (m_id + ".progress.tmp");
            boost::filesystem::ofstream out(temp, std::ios::binary);
            out << PauseAMS::serialize_pause_progress(m_progress, m_timeline.events.size()); out.close();
            if (!out) return false;
            const auto dest = m_folder / (m_id + ".progress");
            // Windows rename cannot replace an existing file. A lost local checkpoint
            // is recoverable using Adjust progress; no printer state is changed.
            if (boost::filesystem::exists(dest)) boost::filesystem::remove(dest);
            boost::filesystem::rename(temp, dest); return true;
        } catch (...) { return false; }
    }
    bool same_run_now() const
    {
        try { return m_run.empty() || read_file(m_folder / (m_id + ".run"), 512) == m_run; }
        catch (...) { return false; }
    }
    void render()
    {
        if (!m_spec) return;
        const bool online = m_state.connected && m_state.ready;
        const bool active = m_state.status == "RUNNING" || m_state.status == "PAUSE";
        // The saved guide is static, so it can be read while the connection is down; only confirming needs a live printer.
        m_show->Enable(!m_timeline.events.empty()); m_adjust->Enable(online && active);
        m_show->SetLabel(m_progress.awaiting_resume ? "Show completed pause" :
            (m_state.status == "PAUSE" ? "Show what to change" : "Show instructions"));
        wxString title = "Next unconfirmed pause";
        wxString text;
        if (!online) title = "Pause overview | Printer disconnected";
        else if (!active) title = "Pause overview | Print is not running";
        else if (PauseAMS::is_printer_fault(m_state.error)) title = "Printer error | Check printer first";
        else if (m_progress.awaiting_resume) title = "Pause marked complete | Resume on the printer";
        else if (m_state.status == "PAUSE") title = "Printer paused | Check the planned stop below";
        if (m_progress.completed < m_timeline.events.size()) {
            const auto& next = m_timeline.events[m_progress.completed];
            text = event_label(next);
            if (online && active && next.layer && next.layer < m_state.layer)
                text += "\nThis planned stop is earlier than the current layer. Check Adjust progress.";
            const int seconds = PauseAMS::estimated_seconds_to_pause(next, m_state);
            if (seconds >= 0 && (!next.layer || next.layer >= m_state.layer))
                text += wxString::Format(" | About %d min (estimate)", (seconds + 59) / 60);
        } else text = "All listed pauses marked complete.";
        text += wxString::Format("\n%d completed | %d remaining", int(m_progress.completed), int(m_timeline.events.size() - m_progress.completed));
        if (!m_timeline.complete) {
            text += "\nOlder job: magnet pauses are separate and not included above. Insert magnets at Z:";
            for (double z : m_spec->magnet_pauses) text += wxString::Format(" %.2f mm", z);
            text += ". Keep all spools loaded at those pauses.";
        }
        if (!m_saved_progress) text += "\nProgress is local to this session. Use Adjust progress if the print is already underway.";
        text += "\nA manual pause or error does not identify a planned stop. Progress is confirmed manually.";
        labels(title, text);
    }
    void instructions()
    {
        if (!m_spec || m_timeline.events.empty()) return;
        // Native status timers continue while a modal dialog is open. Snapshot the
        // displayed guide and revalidate identity and progress before accepting it.
        const auto key = m_key;
        const auto completed = m_progress.completed;
        const auto spec = *m_spec;
        const std::size_t index = m_progress.awaiting_resume && completed ? completed - 1 :
            std::min(completed, m_timeline.events.size() - 1);
        const auto event = m_timeline.events[index];
        wxDialog dialog(this, wxID_ANY, "Pause instructions", wxDefaultPosition, wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* heading = new wxStaticText(&dialog, wxID_ANY, event_label(event));
        heading->SetFont(heading->GetFont().Bold().Larger()); root->Add(heading, 0, wxALL, FromDIP(14));
        if (event.kind == PauseAMS::PauseKind::Magnets) {
            auto* text = new wxStaticText(&dialog, wxID_ANY,
                "Insert the magnets fully into their pockets. Keep all four spools loaded.\nCheck that every magnet sits below the next layer, then resume on the printer.");
            text->Wrap(FromDIP(650)); root->Add(text, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
        } else {
            auto loaded = spec.plan.schedule.initial_slots;
            for (std::size_t i = 0; i < event.visit; ++i)
                for (const auto& change : spec.plan.schedule.stops[i].replacements) loaded[change.slot] = change.insert;
            auto* grid = new wxGridSizer(2, FromDIP(10), FromDIP(10));
            const auto& stop = spec.plan.schedule.stops[event.visit];
            // One plain line first: which slots change and which stay, so no card has to be read to know.
            wxString replace_slots, keep_slots;
            for (std::size_t slot = 0; slot < loaded.size(); ++slot) {
                const bool replaced = std::any_of(stop.replacements.begin(), stop.replacements.end(),
                    [slot](const auto& r) { return r.slot == slot; });
                auto& list = replaced ? replace_slots : keep_slots;
                list += (list.empty() ? "" : ", ") + wxString::Format("A%d", int(slot + 1));
            }
            auto* summary = new wxStaticText(&dialog, wxID_ANY, replace_slots.empty() ?
                wxString("No spool needs changing at this pause. Keep all four as they are.") :
                wxString::Format("Replace %d of %d: %s      Keep as is: %s", int(stop.replacements.size()), int(loaded.size()),
                                 replace_slots, keep_slots.empty() ? wxString("none (all four change)") : keep_slots));
            summary->SetFont(summary->GetFont().Bold());
            root->Add(summary, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
            auto filament = [&](int id) {
                if (id < 0) return wxString("Empty");
                const auto& f = spec.filaments.at(std::size_t(id));
                return wxString::FromUTF8(PauseAMS::color_name(f.color) + " | V" + std::to_string(id + 1) + " | " + f.material);
            };
            for (std::size_t slot = 0; slot < loaded.size(); ++slot) {
                const auto change = std::find_if(stop.replacements.begin(), stop.replacements.end(),
                    [slot](const auto& r) { return r.slot == slot; });
                const bool replace = change != stop.replacements.end();
                const int inserted = replace ? change->insert : loaded[slot];
                auto* card = new wxPanel(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
                auto* box = new wxBoxSizer(wxVERTICAL);
                auto* swatch = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(265), FromDIP(35)), wxBORDER_SIMPLE);
                if (inserted >= 0) {
                    const auto& color = spec.filaments.at(std::size_t(inserted)).color;
                    const wxColour rgb(wxString::FromUTF8(color.substr(0, 7)));
                    if (rgb.IsOk()) swatch->SetBackgroundColour(rgb);
                }
                box->Add(swatch, 0, wxEXPAND | wxALL, FromDIP(10));
                auto* name = new wxStaticText(card, wxID_ANY, wxString::Format("Slot A%d", int(slot + 1)));
                name->SetFont(name->GetFont().Bold()); box->Add(name, 0, wxLEFT | wxRIGHT, FromDIP(10));
                auto* verdict = new wxStaticText(card, wxID_ANY, replace ? "REPLACE this spool" : "KEEP - do not touch");
                verdict->SetFont(verdict->GetFont().Bold().Larger());
                verdict->SetForegroundColour(replace ? wxColour(210, 80, 20) : wxColour(40, 150, 70));
                box->Add(verdict, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
                wxString text = replace ? "Take out: " + filament(change->remove) + "\nPut in: " + filament(inserted) :
                    "Already loaded, leave it: " + filament(inserted);
                if (inserted >= 0) text += "\n" + wxString::FromUTF8(spec.filaments[inserted].color);
                auto* label = new wxStaticText(card, wxID_ANY, text); label->Wrap(FromDIP(265));
                box->Add(label, 1, wxEXPAND | wxALL, FromDIP(10)); card->SetSizer(box); grid->Add(card, 1, wxEXPAND);
            }
            root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
            auto* text = new wxStaticText(&dialog, wxID_ANY,
                "The printer has unloaded the active filament. Swap only the slots marked REPLACE and feed each new spool\nto the AMS inlet. Leave the slots marked KEEP alone, then resume on the printer.");
            text->Wrap(FromDIP(650)); root->Add(text, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
        }
        auto* note = new wxStaticText(&dialog, wxID_ANY,
            "Confirm this is the planned stop, rather than a manual pause or printer error.\nMarking complete updates this guide only; it does not resume the printer.");
        note->Wrap(FromDIP(650)); root->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* done = new wxButton(&dialog, wxID_OK, "Mark this pause completed");
        // A greyed button always says why, instead of silently refusing.
        const auto blocker = index != completed ? std::string("This is not the next unconfirmed stop.") :
            PauseAMS::pause_completion_blocker(m_progress, m_timeline, spec, m_target, current_observation());
        done->Enable(blocker.empty());
        buttons->Add(done, 0, wxRIGHT, FromDIP(12));
        if (!blocker.empty()) {
            auto* why = new wxStaticText(&dialog, wxID_ANY, wxString::FromUTF8(blocker)); why->Wrap(FromDIP(420));
            buttons->Add(why, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        }
        auto* close = new wxButton(&dialog, wxID_CANCEL, "Close"); close->SetDefault(); buttons->Add(close);
        root->Add(buttons, 0, wxALIGN_RIGHT | wxALL, FromDIP(14)); dialog.SetSizerAndFit(root);
        wxGetApp().UpdateDarkUI(&dialog); dialog.CenterOnParent();
        if (dialog.ShowModal() == wxID_OK) {
            if (key != m_key || completed != m_progress.completed || !same_run_now() ||
                !PauseAMS::complete_pause(m_progress, m_timeline, *m_spec, m_target, current_observation())) {
                wxMessageBox("The printer state or guide changed. Reopen the instructions to check this stop.", "Pause overview", wxOK, this); return;
            }
            m_saved_progress = !m_run.empty() && save_progress(); render();
        }
    }
    void adjust()
    {
        if (!m_spec) return;
        const auto key = m_key; wxArrayString choices;
        for (const auto& e : m_timeline.events) choices.Add(event_label(e));
        choices.Add("All listed pauses completed");
        wxSingleChoiceDialog dialog(this,
            "Select the NEXT pause that still needs to be completed.\nThis changes only the local guide, not the printer or G-code.",
            "Adjust pause progress", choices);
        dialog.SetSelection(int(m_progress.completed));
        if (dialog.ShowModal() != wxID_OK || key != m_key || !same_run_now()) return;
        const auto state = current_observation();
        if (!PauseAMS::matches_pause_job(*m_spec, m_target, state) || !PauseAMS::matches_pause_run(m_progress, state) || !state.connected || !state.ready ||
            (state.status != "RUNNING" && state.status != "PAUSE")) return;
        m_progress.completed = std::size_t(dialog.GetSelection()); m_progress.awaiting_resume = false;
        m_saved_progress = !m_run.empty() && save_progress(); render();
    }
public:
    explicit ExtendedAMSPauseOverview(wxWindow* parent) : wxPanel(parent)
    {
        SetName(pause_overview_name);
        m_folder = boost::filesystem::path(data_dir()) / "extended_ams_jobs";
        auto* root = new wxBoxSizer(wxVERTICAL);
        m_title = new wxStaticText(this, wxID_ANY, "Next unconfirmed pause"); m_title->SetFont(m_title->GetFont().Bold());
        m_detail = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        root->Add(m_detail, 0, wxEXPAND | wxALL, FromDIP(12));
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_show = new wxButton(this, wxID_ANY, "Show instructions"); m_adjust = new wxButton(this, wxID_ANY, "Adjust progress...");
        row->Add(m_show, 0, wxRIGHT, FromDIP(8)); row->Add(m_adjust);
        root->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12)); SetSizer(root); Hide();
        m_show->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { instructions(); });
        m_adjust->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { adjust(); });
    }
    void update(MachineObject* obj)
    {
        m_state = observation(obj);
        // Leave a trace when the status link drops or returns, so a "no connection" moment in the
        // middle of a print can be matched to a time in the log. Nothing identifying is written.
        if (obj) {
            const bool online = m_state.connected && m_state.ready;
            if (online != m_was_online) {
                BOOST_LOG_TRIVIAL(info) << "Extended AMS Lite: printer status link " << (online ? "restored" : "lost")
                    << " (connected=" << m_state.connected << ", info ready=" << m_state.ready
                    << ", status=" << m_state.status << ", layer=" << m_state.layer << ")";
                m_was_online = online;
            }
        }
        const auto id = PauseAMS::pause_job_id(m_state.job_name);
        if (id.empty()) { set_visible(false); return; }
        try {
            const auto run = read_file(m_folder / (id + ".run"), 512, true);
            if (!PauseAMS::matches_pause_run({run, 0, false}, m_state)) { set_visible(false); return; }
            const auto key = m_state.printer + ":" + id + ":" + run;
            if (key != m_key) {
                const auto target = read_file(m_folder / (id + ".target"), 512);
                if (target != m_state.printer) { set_visible(false); return; }
                auto spec = PauseAMS::deserialize_execution(read_file(m_folder / (id + ".plan"), 10000000));
                if (spec.job_id != id) throw std::runtime_error("Saved guide has a different job identity.");
                const auto text = read_file(m_folder / (id + ".timeline"), 10000000, true);
                auto timeline = text.empty() ? PauseAMS::legacy_pause_timeline(spec) : PauseAMS::deserialize_pause_timeline(text, spec);
                PauseAMS::PauseProgress progress; progress.run = run;
                bool saved = !run.empty();
                if (!run.empty()) {
                    const auto checkpoint = read_file(m_folder / (id + ".progress"), 2048, true);
                    try { if (!checkpoint.empty()) { progress = PauseAMS::deserialize_pause_progress(checkpoint, run, timeline.events.size()); saved = true; } }
                    catch (...) { /* A previous submission's checkpoint never carries into a new run. */ }
                }
                m_spec = std::move(spec); m_timeline = std::move(timeline); m_progress = std::move(progress);
                m_target = target; m_key = key; m_id = id; m_run = run; m_saved_progress = saved;
            }
            if (PauseAMS::observe_pause_progress(m_progress, *m_spec, m_target, m_state))
                m_saved_progress = !m_run.empty() && save_progress();
            set_visible(true); render();
        } catch (...) {
            m_spec.reset(); m_key.clear(); m_show->Disable(); m_adjust->Disable(); set_visible(true);
            labels("Pause overview unavailable", "The saved guide could not be verified for this print. Use the original swap schedule.");
        }
    }
};
} // namespace
} // namespace Slic3r::GUI

#include "WhiteLayerDialog.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "I18N.hpp"
#include "libslic3r/WhiteLayer.hpp"
#include "libslic3r/ExtendedAMSLite/Execution.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <algorithm>

namespace Slic3r::GUI {
namespace WL = WhiteLayer;
namespace {
class WhiteLayerDialog : public DPIDialog {
public:
    WhiteLayerDialog(wxWindow* parent, const ModelObject& object, std::size_t instance, DynamicPrintConfig config)
        : DPIDialog(parent, wxID_ANY, _L("White layer"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_object(object), m_instance(instance), m_config(std::move(config))
    {
        WL::Options defaults;
        for (const auto* v : object.volumes) if (WL::is_white_layer(*v)) { defaults = WL::options_from_volume(*v); m_existing = true; break; }
        const auto* colours = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const std::size_t count = colours ? colours->values.size() : 0;
        if (!m_existing) defaults.filament = nearest_white(colours);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY, _L("Print some layers behind the colored face in white. Layers are counted from the bed, so the face must be on the plate."));
        title->SetFont(title->GetFont().Bold()); title->Wrap(60 * em_unit());
        root->Add(title, 0, wxEXPAND | wxALL, em_unit());
        auto* fields = new wxFlexGridSizer(2, em_unit() / 2, em_unit()); fields->AddGrowableCol(1);
        auto row = [&](const wxString& text, wxWindow* control) {
            fields->Add(new wxStaticText(this, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
            fields->Add(control, 1, wxEXPAND);
        };
        m_color = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10 * em_unit(), -1), wxSP_ARROW_KEYS, 0, 200, int(defaults.color_layers));
        row(_L("Color layers to keep (from the bed)"), m_color);
        m_white = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10 * em_unit(), -1), wxSP_ARROW_KEYS, 1, 100, int(defaults.white_layers));
        row(_L("White layers behind them"), m_white);
        m_filament = new wxChoice(this, wxID_ANY);
        for (std::size_t i = 0; i < count; ++i) m_filament->Append(wxString::Format(_L("Filament %d  (%s)"), int(i + 1), wxString::FromUTF8(colours->values[i])));
        m_filament->SetSelection(std::clamp(defaults.filament - 1, 0, std::max(0, int(count) - 1)));
        row(_L("White filament"), m_filament);
        root->Add(fields, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_info, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* note = new wxStaticText(this, wxID_ANY, _L("Everything in those layers prints in the white filament, including parts that have their own color. No plastic is added. Slice again afterwards, and prepare the AMS Lite swap plan again if you use swaps."));
        note->Wrap(60 * em_unit());
        root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_apply = new wxButton(this, wxID_OK, m_existing ? _L("Update white layer") : _L("Add white layer"));
        buttons->Add(m_apply, 0, wxRIGHT, em_unit());
        if (m_existing) {
            auto* remove = new wxButton(this, wxID_REMOVE, _L("Remove white layer"));
            remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_REMOVE); });
            buttons->Add(remove, 0, wxRIGHT, em_unit());
        }
        buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")));
        root->Add(buttons, 0, wxALIGN_RIGHT | wxALL, em_unit());
        SetSizerAndFit(root);
        wxGetApp().UpdateDarkUI(this);
        CenterOnParent();
        auto changed = [this](wxCommandEvent&) { refresh(); };
        m_color->Bind(wxEVT_SPINCTRL, changed); m_white->Bind(wxEVT_SPINCTRL, changed);
        m_color->Bind(wxEVT_TEXT, changed); m_white->Bind(wxEVT_TEXT, changed);
        m_filament->Bind(wxEVT_CHOICE, changed);
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_plan) EndModal(wxID_OK); });
        refresh();
    }
    const WL::Plan& result() const { return *m_plan; }
protected:
    void on_dpi_changed(const wxRect&) override { Layout(); Refresh(); }
private:
    const ModelObject& m_object;
    std::size_t m_instance;
    DynamicPrintConfig m_config;
    bool m_existing = false;
    wxSpinCtrl *m_color, *m_white;
    wxChoice* m_filament;
    wxStaticText* m_info;
    wxButton* m_apply;
    std::optional<WL::Plan> m_plan;
    // The filament closest to pure white, so the common case needs no thought.
    static int nearest_white(const ConfigOptionStrings* colours)
    {
        int best = 1, best_distance = 1 << 20;
        if (!colours) return best;
        for (std::size_t i = 0; i < colours->values.size(); ++i) {
            wxColour c(wxString::FromUTF8(colours->values[i]));
            if (!c.IsOk()) continue;
            const int distance = (255 - c.Red()) + (255 - c.Green()) + (255 - c.Blue());
            if (distance < best_distance) { best_distance = distance; best = int(i + 1); }
        }
        return best;
    }
    void refresh()
    {
        m_plan.reset();
        try {
            WL::Options o;
            o.color_layers = unsigned(std::max(0, m_color->GetValue()));
            o.white_layers = unsigned(std::max(1, m_white->GetValue()));
            o.filament = m_filament->GetSelection() + 1;
            m_plan = WL::plan(m_object, m_instance, m_config, o);
            m_info->SetLabel(wxString::Format(_L("Layers %u to %u print in white (%.2f to %.2f mm above the bed). The model has %u layers."),
                o.color_layers + 1, o.color_layers + o.white_layers, m_plan->bottom_z, m_plan->top_z, m_plan->total_layers));
        } catch (const std::exception& error) {
            m_info->SetLabel(wxString::FromUTF8(error.what()));
        }
        m_info->Wrap(60 * em_unit());
        m_apply->Enable(m_plan.has_value() && m_filament->GetSelection() >= 0);
        Layout();
    }
};
}

void show_white_layer(wxWindow* parent)
{
    auto* plater = wxGetApp().plater();
    const auto& selection = plater->get_selection();
    const int id = wxGetApp().obj_list()->get_selected_obj_idx();
    if (id < 0 || std::size_t(id) >= plater->model().objects.size() || selection.get_instance_idxs().empty()) {
        wxMessageBox(_L("Select one model in Prepare first."), _L("White layer"), wxOK | wxICON_INFORMATION, parent); return;
    }
    const int instance = *selection.get_instance_idxs().begin();
    auto* plate = plater->get_partplate_list().get_curr_plate();
    try {
        auto config = wxGetApp().preset_bundle->full_config(); config.apply(*plate->config());
        WhiteLayerDialog dialog(parent, *plater->model().objects[id], std::size_t(instance), config);
        const int action = dialog.ShowModal();
        if (action != wxID_OK && action != wxID_REMOVE) return;
        // Validate on a copy first so a failure never leaves the real model half-edited.
        Model candidate = plater->model();
        if (action == wxID_REMOVE) WL::remove(*candidate.objects[id]);
        else WL::apply(*candidate.objects[id], std::size_t(instance), dialog.result());
        plater->take_snapshot(action == wxID_REMOVE ? "Remove white layer" : "Add or update white layer");
        auto* original = plater->model().objects[id];
        if (action == wxID_REMOVE) WL::remove(*original);
        else WL::apply(*original, std::size_t(instance), dialog.result());
        auto& preset = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const auto notes = preset.opt_string("process_notes");
        const bool had_plan = notes.find("[[EXTENDED_AMS_LITE_") != std::string::npos;
        if (had_plan) {
            // A saved swap plan describes the old layer colors; mark it stale instead of letting it fail later.
            preset.set_key_value("process_notes", new ConfigOptionString(ExtendedAMSLite::notes_with_execution(notes, std::nullopt) + "\n" + ExtendedAMSLite::execution_preparing_tag));
            wxGetApp().preset_bundle->prints.update_dirty();
            wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
            plater->on_config_change(wxGetApp().preset_bundle->full_config());
        }
        auto items = wxGetApp().obj_list()->reorder_volumes_and_get_selection(id, [](const ModelVolume* v) { return WL::is_white_layer(*v); });
        if (!items.empty()) wxGetApp().obj_list()->select_items(items);
        wxGetApp().obj_list()->notify_instance_updated(id);
        plater->changed_object(id); plater->set_plater_dirty(true); plater->update();
        wxMessageBox(had_plan ? _L("The white layer is updated. Slice, then Prepare print again to update the AMS spool-swap plan.") :
                                _L("The white layer is updated. Slice again to see it in the preview."),
                     _L("White layer"), wxOK | wxICON_INFORMATION, parent);
    } catch (const std::exception& error) {
        wxMessageBox(wxString::FromUTF8(error.what()), _L("White layer"), wxOK | wxICON_ERROR, parent);
    }
}
}

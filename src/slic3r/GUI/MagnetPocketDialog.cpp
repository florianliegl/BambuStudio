#include "MagnetPocketDialog.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "I18N.hpp"
#include "libslic3r/MagnetPockets.hpp"
#include "libslic3r/ExtendedAMSLite/Execution.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <optional>
#include <atomic>
#include <chrono>
#include <thread>

namespace Slic3r::GUI {
namespace MP = MagnetPockets;
namespace {
class MagnetPocketDialog : public DPIDialog {
public:
    MagnetPocketDialog(wxWindow* parent, const ModelObject& object, std::size_t instance, DynamicPrintConfig config)
        : DPIDialog(parent, wxID_ANY, _L("Magnet pockets"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_instance(instance), m_config(std::move(config)), m_timer(this)
    {
        m_snapshot = std::make_shared<Model>();
        m_snapshot->add_object(object);
        MP::Options defaults;
        m_existing = MP::pocket_centers(object, instance);
        for (const auto* v : object.volumes) if (MP::is_pocket(*v)) { defaults = MP::options_from_pocket(*v); break; }
        if (!m_existing.empty()) defaults.count = unsigned(m_existing.size());
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY, _L("Choose your magnets. The pockets and insertion pause are placed for you."));
        title->SetFont(title->GetFont().Bold()); title->Wrap(65 * em_unit());
        root->Add(title, 0, wxEXPAND | wxALL, em_unit());
        auto* columns = new wxBoxSizer(wxHORIZONTAL);
        auto* fields = new wxFlexGridSizer(2, em_unit() / 2, em_unit()); fields->AddGrowableCol(1);
        auto label = [&](const wxString& text, wxWindow* control) {
            fields->Add(new wxStaticText(this, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
            fields->Add(control, 1, wxEXPAND);
        };
        m_preset = new wxChoice(this, wxID_ANY);
        for (const auto& text : {"Custom size", "3 x 1 mm", "5 x 2 mm", "6 x 2 mm", "8 x 2 mm", "10 x 2 mm", "12 x 3 mm"}) m_preset->Append(wxString::FromUTF8(text));
        m_preset->SetSelection(defaults.diameter == 8. && defaults.thickness == 2. ? 4 : 0);
        label(_L("Round magnet preset"), m_preset);
        auto number = [&](const wxString& text, double value, double low, double high) {
            auto* control = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(13 * em_unit(), -1), wxSP_ARROW_KEYS, low, high, value, 0.05);
            control->SetDigits(2); label(text, control);
            control->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { changed(); });
            control->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { changed(); });
            return control;
        };
        m_diameter = number(_L("Magnet diameter (mm)"), defaults.diameter, 0.5, 200.);
        m_thickness = number(_L("Magnet thickness (mm)"), defaults.thickness, 0.1, 100.);
        m_count = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 1000, defaults.count);
        label(_L("Number of magnets"), m_count);
        m_cover = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100, defaults.cover_layers);
        label(_L("Closing layers above magnets"), m_cover);
        m_dclear = number(_L("Extra pocket diameter (mm)"), defaults.diameter_allowance, 0., 5.);
        m_hclear = number(_L("Extra pocket depth (mm)"), defaults.depth_allowance, 0., 5.);
        m_edge = number(_L("Minimum side/between wall (mm)"), defaults.edge_wall, 0.2, 20.);
        m_floor = number(_L("Minimum bottom wall (mm)"), defaults.bottom_wall, 0.1, 20.);
        columns->Add(fields, 0, wxEXPAND | wxRIGHT, em_unit());
        m_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(31 * em_unit(), 28 * em_unit()), wxBORDER_SIMPLE);
        m_preview->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_preview->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        columns->Add(m_preview, 1, wxEXPAND);
        root->Add(columns, 1, wxEXPAND | wxLEFT | wxRIGHT, em_unit());
        m_keep = new wxCheckBox(this, wxID_ANY, _L("Keep my existing pocket positions when updating size or height"));
        m_keep->SetValue(!m_existing.empty()); m_keep->Enable(!m_existing.empty());
        root->Add(m_keep, 0, wxEXPAND | wxALL, em_unit());
        m_no_support = new wxCheckBox(this, wxID_ANY, _L("Keep the cavities empty: disable supports on this object"));
        m_no_support->SetValue(true); root->Add(m_no_support, 0, wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* help = new wxStaticText(this, wxID_ANY, _L("After Apply, move individual pockets with Studio's normal Move tool. The pause is before the first closing layer. Insert every magnet flush with its pocket, then Resume. The tool affects all copies of this object. Existing manual negative parts are kept."));
        help->Wrap(65 * em_unit()); root->Add(help, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em_unit());
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* remove = new wxButton(this, wxID_REMOVE, _L("Remove pockets")); remove->Enable(!m_existing.empty());
        m_apply = new wxButton(this, wxID_OK, _L("Apply pockets && pause"));
        buttons->Add(remove); buttons->AddStretchSpacer(); buttons->Add(m_apply, 0, wxRIGHT, em_unit());
        buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")));
        root->Add(buttons, 0, wxEXPAND | wxALL, em_unit());
        SetSizerAndFit(root); SetMinSize(GetSize()); CenterOnParent(); wxGetApp().UpdateDarkUI(this);
        m_count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { if (m_count->GetValue() != int(m_existing.size())) m_keep->SetValue(false); changed(); });
        m_count->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { if (m_count->GetValue() != int(m_existing.size())) m_keep->SetValue(false); changed(); });
        m_cover->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { changed(); });
        m_cover->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { changed(); });
        m_keep->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { changed(); });
        m_preset->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            const double sizes[][2] = {{8,2},{3,1},{5,2},{6,2},{8,2},{10,2},{12,3}};
            const int i = m_preset->GetSelection();
            if (i > 0) { m_diameter->SetValue(sizes[i][0]); m_thickness->SetValue(sizes[i][1]); changed(); }
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { calculate(); }, m_timer.GetId());
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_plan && m_completed_revision == m_revision) EndModal(wxID_OK); });
        remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_REMOVE); });
        changed();
    }
    ~MagnetPocketDialog() override { m_timer.Stop(); if (m_job) m_job->canceled = true; }
    const MP::Plan& result() const { return *m_plan; }
    bool disable_support() const { return m_no_support->GetValue(); }
protected:
    void on_dpi_changed(const wxRect&) override { Layout(); Refresh(); }
private:
    struct Job {
        std::atomic<bool> done{false}, canceled{false};
        std::size_t revision = 0;
        std::optional<MP::Plan> plan;
        std::string error;
    };
    std::shared_ptr<Model> m_snapshot;
    std::shared_ptr<Job> m_job;
    std::size_t m_instance; DynamicPrintConfig m_config;
    std::size_t m_revision = 0, m_completed_revision = 0;
    std::chrono::steady_clock::time_point m_not_before;
    wxTimer m_timer;
    wxChoice* m_preset; wxSpinCtrl *m_count, *m_cover;
    wxSpinCtrlDouble *m_diameter, *m_thickness, *m_dclear, *m_hclear, *m_edge, *m_floor;
    wxCheckBox *m_keep, *m_no_support; wxPanel* m_preview; wxStaticText* m_summary = nullptr; wxButton* m_apply = nullptr;
    std::vector<Vec2d> m_existing; std::optional<MP::Plan> m_plan;
    void changed() {
        ++m_revision;
        if (m_apply) m_apply->Disable();
        if (m_job) m_job->canceled = true;
        m_not_before = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
        if (m_summary) m_summary->SetLabel(_L("Updating pocket preview..."));
        m_timer.StartOnce(m_job ? 30 : 350);
    }
    void calculate() {
        m_timer.Stop();
        if (m_job) {
            if (!m_job->done.load(std::memory_order_acquire)) { m_timer.StartOnce(30); return; }
            auto completed = std::move(m_job);
            if (completed->revision == m_revision && !completed->canceled) {
                m_completed_revision = m_revision;
                m_plan = std::move(completed->plan);
                if (m_plan) {
                    const auto& o = m_plan->options;
                    m_summary->SetLabel(wxString::Format(_L("%u pockets: %.2f mm diameter x %.2f mm deep. Center Z %.2f mm.\nPause before object layer %u of %u (Z %.2f mm), then print %u closing layers."),
                        o.count, m_plan->diameter, m_plan->depth, (m_plan->bottom_z + m_plan->top_z) / 2.,
                        m_plan->cover_layer, m_plan->total_layers, m_plan->pause_z, o.cover_layers));
                    m_apply->Enable();
                } else m_summary->SetLabel(wxString::FromUTF8(completed->error));
                m_summary->Wrap(65 * em_unit()); m_preview->Refresh(); Layout();
                return;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < m_not_before) { m_timer.StartOnce(50); return; }
        MP::Options o; o.diameter = m_diameter->GetValue(); o.thickness = m_thickness->GetValue();
        o.count = m_count->GetValue(); o.cover_layers = m_cover->GetValue();
        o.diameter_allowance = m_dclear->GetValue(); o.depth_allowance = m_hclear->GetValue();
        o.edge_wall = m_edge->GetValue(); o.bottom_wall = m_floor->GetValue();
        const auto centers = m_keep->GetValue() ? m_existing : std::vector<Vec2d>{};
        const auto snapshot = m_snapshot;
        const auto config = m_config;
        const auto instance = m_instance;
        auto job = std::make_shared<Job>(); job->revision = m_revision; m_job = job;
        // No wx calls or references to the dialog/model being edited on this
        // worker. Closing the dialog cancels safely without waiting for it.
        std::thread([job, snapshot, config, instance, o, centers] {
            try {
                job->plan = MP::plan(*snapshot->objects.front(), instance, config, o, centers, [job] {
                    if (job->canceled) throw std::runtime_error("Pocket preview canceled.");
                });
            } catch (const std::exception& error) { job->error = error.what(); }
            job->done.store(true, std::memory_order_release);
        }).detach();
        m_timer.StartOnce(30);
    }
    void paint() {
        wxAutoBufferedPaintDC dc(m_preview); dc.SetBackground(wxBrush(wxColour(245, 245, 245))); dc.Clear();
        if (!m_plan) { dc.DrawText(_L("Adjust the size or count to see a placement."), 12, 12); return; }
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc)); if (!gc) return;
        BoundingBox bounds; for (const auto& polygon : m_plan->footprint) bounds.merge(polygon.contour.bounding_box());
        const auto sz = m_preview->GetClientSize();
        const double scale = std::min((sz.x - 30.) / unscale<double>(bounds.size().x()), (sz.y - 30.) / unscale<double>(bounds.size().y()));
        auto point = [&](const Point& p) { return wxPoint2DDouble(15 + unscale<double>(p.x() - bounds.min.x()) * scale, sz.y - 15 - unscale<double>(p.y() - bounds.min.y()) * scale); };
        auto path = gc->CreatePath();
        for (const auto& shape : m_plan->footprint) {
            auto polygon = [&](const Polygon& poly) { if (poly.points.empty()) return; auto a = point(poly.points.front()); path.MoveToPoint(a.m_x, a.m_y); for (const auto& p : poly.points) { a = point(p); path.AddLineToPoint(a.m_x, a.m_y); } path.CloseSubpath(); };
            polygon(shape.contour); for (const auto& hole : shape.holes) polygon(hole);
        }
        gc->SetBrush(wxBrush(wxColour(205, 210, 215))); gc->SetPen(wxPen(wxColour(95, 100, 105)));
        gc->DrawPath(path, wxODDEVEN_RULE);
        gc->SetBrush(wxBrush(wxColour(85, 205, 150))); gc->SetPen(wxPen(wxColour(10, 130, 80), 2));
        for (std::size_t i = 0; i < m_plan->centers.size(); ++i) {
            const auto& center = m_plan->centers[i];
            auto a = point(Point(scale_(center.x()), scale_(center.y())));
            const double d = m_plan->diameter * scale;
            gc->DrawEllipse(a.m_x - d/2., a.m_y - d/2., d, d);
            if (d > 16) dc.DrawText(wxString::Format("%zu", i + 1), int(a.m_x - 4), int(a.m_y - 7));
        }
    }
};
}
void show_magnet_pockets(wxWindow* parent)
{
    auto* plater = wxGetApp().plater();
    const auto& selection = plater->get_selection();
    const int id = wxGetApp().obj_list()->get_selected_obj_idx();
    if (id < 0 || std::size_t(id) >= plater->model().objects.size() || selection.get_instance_idxs().empty()) {
        wxMessageBox(_L("Select one keychain object in Prepare first."), _L("Magnet pockets"), wxOK | wxICON_INFORMATION, parent); return;
    }
    const int instance = *selection.get_instance_idxs().begin();
    auto* plate = plater->get_partplate_list().get_curr_plate();
    try {
        auto config = wxGetApp().preset_bundle->full_config(); config.apply(*plate->config());
        MagnetPocketDialog dialog(parent, *plater->model().objects[id], std::size_t(instance), config);
        const int action = dialog.ShowModal();
        if (action != wxID_OK && action != wxID_REMOVE) return;
        Model candidate = plater->model();
        auto* object = candidate.objects[id];
        if (action == wxID_REMOVE) MP::remove(*object);
        else {
            MP::apply(*object, std::size_t(instance), dialog.result());
            if (dialog.disable_support()) object->config.set_key_value("enable_support", new ConfigOptionBool(false));
        }
        std::vector<int> object_ids;
        for (const auto* on_plate : plate->get_objects_on_this_plate()) {
            const auto it = std::find(plater->model().objects.begin(), plater->model().objects.end(), on_plate);
            if (it != plater->model().objects.end()) object_ids.push_back(int(it - plater->model().objects.begin()));
        }
        MP::sync_pauses(candidate, plate->get_index(), object_ids, config);
        plater->take_snapshot(action == wxID_REMOVE ? "Remove magnet pockets" : "Add or update magnet pockets");
        auto* original = plater->model().objects[id];
        if (action == wxID_REMOVE) MP::remove(*original);
        else {
            MP::apply(*original, std::size_t(instance), dialog.result());
            if (dialog.disable_support()) original->config.set_key_value("enable_support", new ConfigOptionBool(false));
        }
        plater->model().plates_custom_gcodes[plate->get_index()] = candidate.plates_custom_gcodes[plate->get_index()];
        auto& preset = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const auto notes = preset.opt_string("process_notes");
        if (notes.find("[[EXTENDED_AMS_LITE_") != std::string::npos) {
            preset.set_key_value("process_notes", new ConfigOptionString(ExtendedAMSLite::notes_with_execution(notes, std::nullopt) + "\n" + ExtendedAMSLite::execution_preparing_tag));
            wxGetApp().preset_bundle->prints.update_dirty();
            wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
            plater->on_config_change(wxGetApp().preset_bundle->full_config());
        }
        auto items = wxGetApp().obj_list()->reorder_volumes_and_get_selection(id, [](const ModelVolume* v) { return MP::is_pocket(*v); });
        if (!items.empty()) wxGetApp().obj_list()->select_items(items);
        wxGetApp().obj_list()->notify_instance_updated(id);
        plater->changed_object(id); plater->set_plater_dirty(true); plater->update();
        if (notes.find("[[EXTENDED_AMS_LITE_") != std::string::npos)
            wxMessageBox(_L("Pockets and the magnet pause are updated. Slice, then Prepare print again to update the AMS spool-swap plan."), _L("Magnet pockets"), wxOK | wxICON_INFORMATION, parent);
    } catch (const std::exception& error) {
        wxMessageBox(wxString::FromUTF8(error.what()), _L("Magnet pockets"), wxOK | wxICON_ERROR, parent);
    }
}
}

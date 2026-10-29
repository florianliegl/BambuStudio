//#include "stdlib.h"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "IMSlider.hpp"
#include "IMSlider_Utils.hpp"
#include "GUI_Preview.hpp"
#include "ExtendedAMSLiteDialog.hpp"
#include "Tab.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "libslic3r/ExtendedAMSLite/Execution.hpp"
#include "libslic3r/ExtendedAMSLite/Connect.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <set>
#include <fstream>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "3DScene.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "OpenGLManager.hpp"
#include "GLCanvas3D.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "GCodeViewer.hpp"
#include "slic3r/GUI/GCodeRenderer/BaseRenderer.hpp"

#include <wx/listbook.h>
#include <wx/notebook.h>
#include <wx/glcanvas.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/combo.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/timer.h>
#include <wx/weakref.h>

// this include must follow the wxWidgets ones or it won't compile on Windows -> see http://trac.wxwidgets.org/ticket/2421
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "NotificationManager.hpp"

#ifdef _WIN32
#include "BitmapComboBox.hpp"
#include <wx/msw/registry.h>
#endif

namespace Slic3r {
namespace GUI {

struct Preview::AMSPreparation {
    ExtendedAMSLite::ExecutionSpec spec;
    int plate = -1;
    bool encoded = false, scheduled = false;
};

BaseView::BaseView()
    : wxPanel()
    , m_canvas_widget(nullptr)
    , m_canvas(nullptr)
{
}

BaseView::~BaseView()
{
}

bool BaseView::Show(bool show)
{
    const bool rt = wxPanel::Show(show);
    if (show && rt) {
        if (m_canvas) {
            m_canvas->mark_context_dirty();
        }
    }
    return rt;
}

const std::shared_ptr<Camera>& BaseView::get_override_camera() const
{
    return m_p_override_camera;
}

View3D::View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
    : BaseView()
{
    init(parent, bed, model, config, process);
}

View3D::~View3D()
{
    delete m_canvas;
    delete m_canvas_widget;
}

bool View3D::init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return false;
    }
    const GUI::EMSAAType msaa_type = p_ogl_manager->is_fxaa_enabled() ? GUI::EMSAAType::Disabled : p_ogl_manager->get_msaa_type();
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, msaa_type);
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));

    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    // XXX: If have OpenGL
    m_canvas->enable_picking(true);
    m_canvas->enable_moving(true);
    // XXX: more config from 3D.pm
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasView3D);
    m_canvas->set_config(config);
    m_canvas->enable_gizmos(true);
    m_canvas->enable_selection(true);
    m_canvas->enable_main_toolbar(true);
    m_canvas->enable_return_toolbar(true);
    //BBS: GUI refactor: GLToolbar
    m_canvas->enable_select_plate_toolbar(false);
    m_canvas->enable_labels(true);
    m_canvas->enable_slope(true);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    return true;
}

void View3D::set_as_dirty()
{
    if (m_canvas != nullptr) {
        m_canvas->set_as_dirty();
    }
}

void View3D::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void View3D::plates_count_changed()
{
    if (m_canvas != nullptr)
        m_canvas->plates_count_changed();
}

void View3D::select_view(const std::string& direction)
{
    if (m_canvas != nullptr)
        m_canvas->select_view(direction);
}

//BBS
void View3D::select_curr_plate_all()
{
    if (m_canvas != nullptr)
        m_canvas->select_curr_plate_all();
}

void View3D::select_object_from_idx(std::vector<int>& object_idxs) {
    if (m_canvas != nullptr)
        m_canvas->select_object_from_idx(object_idxs);
}

//BBS
void View3D::remove_curr_plate_all()
{
    if (m_canvas != nullptr)
        m_canvas->remove_curr_plate_all();
}

void View3D::select_all()
{
    if (m_canvas != nullptr)
        m_canvas->select_all();
}

void View3D::deselect_all()
{
    if (m_canvas != nullptr)
        m_canvas->deselect_all();
}

void View3D::exit_gizmo()
{
    if (m_canvas != nullptr)
        m_canvas->exit_gizmo();
}

void View3D::delete_selected()
{
    if (m_canvas != nullptr)
        m_canvas->delete_selected();
}

void View3D::center_selected()
{
    if (m_canvas != nullptr)
        m_canvas->do_center();
}

void View3D::center_selected_plate(const int plate_idx) {
    if (m_canvas != nullptr)
        m_canvas->do_center_plate(plate_idx);
}

void View3D::mirror_selection(Axis axis)
{
    if (m_canvas != nullptr)
        m_canvas->mirror_selection(axis);
}

bool View3D::is_layers_editing_enabled() const
{
    return (m_canvas != nullptr) ? m_canvas->is_layers_editing_enabled() : false;
}

bool View3D::is_layers_editing_allowed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_layers_editing_allowed() : false;
}

void View3D::enable_layers_editing(bool enable)
{
    if (m_canvas != nullptr)
        m_canvas->enable_layers_editing(enable);
}

bool View3D::is_dragging() const
{
    return (m_canvas != nullptr) ? m_canvas->is_dragging() : false;
}

bool View3D::is_reload_delayed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_reload_delayed() : false;
}

void View3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas != nullptr)
        m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
}

void View3D::render()
{
    if (m_canvas != nullptr)
        //m_canvas->render();
        m_canvas->set_as_dirty();
}

Preview::Preview(
    wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config,
    BackgroundSlicingProcess* process, GCodeProcessorResult* gcode_result, std::function<void()> schedule_background_process_func)
    : BaseView()
    , m_config(config)
    , m_process(process)
    , m_gcode_result(gcode_result)
    , m_schedule_background_process(schedule_background_process_func)
{
    if (init(parent, bed, model))
        load_print();
}

void Preview::update_gcode_result(GCodeProcessorResult* gcode_result)
{
    m_gcode_result = gcode_result;

    return;
}

bool Preview::init(wxWindow* parent, Bed3D& bed, Model* model)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

    // to match the background of the sliders
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(this);
#else
    SetBackgroundColour(GetParent()->GetBackgroundColour());
#endif // _WIN32

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return false;
    }
    const auto& ogl_manager = wxGetApp().get_opengl_manager();
    const GUI::EMSAAType msaa_type = p_ogl_manager->is_fxaa_enabled() ? GUI::EMSAAType::Disabled : p_ogl_manager->get_msaa_type();
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, msaa_type);
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));
    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    m_canvas->set_config(m_config);
    m_canvas->set_model(model);
    m_canvas->set_process(m_process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasPreview);

    //BBS: GUI refactor: GLToolbar
    if (wxGetApp().is_editor()) {
        m_canvas->enable_select_plate_toolbar(true);
    }

    // sizer, m_canvas_widget
    m_canvas_widget->Bind(wxEVT_KEY_DOWN, &Preview::update_layers_slider_from_canvas, this);

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    auto* ams_row = new wxBoxSizer(wxHORIZONTAL);
    auto* swaps = new wxButton(this, wxID_ANY, _L("Extended AMS Lite: batch swaps..."));
    ams_row->Add(swaps, 0, wxALL, 5);
    if (wxGetApp().is_editor()) {
        auto* connect = new wxButton(this, wxID_ANY, _L("Open in Bambu Connect..."));
        connect->SetToolTip(_L("Use your Bambu account to print this sliced plate. Select the printer and filament slots in Bambu Connect."));
        ams_row->Add(connect, 0, wxALL, 5);
        connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            auto* plater = wxGetApp().plater();
            auto* plate = plater->get_partplate_list().get_curr_plate();
            if (m_only_gcode || !m_gcode_result || !plate ||
                !plate->is_slice_result_ready_for_export()) {
                wxMessageBox(_L("Slice the current project plate successfully before opening it in Bambu Connect."),
                    _L("Bambu Connect"), wxOK | wxICON_INFORMATION, this);
                return;
            }
            std::set<unsigned int> filaments;
            for (const auto& move : m_gcode_result->moves)
                if (move.type == EMoveType::Extrude) filaments.insert(move.extruder_id);
            const auto& printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
            const auto notes = wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_string("process_notes");
            if (notes.find("[[EXTENDED_AMS_LITE_") != std::string::npos) {
                wxMessageBox(_L("Prepared AMS Lite jobs use fixed slot assignments. Use Print plate with your A1 in LAN-only Developer Mode."), _L("Extended AMS Lite"), wxOK | wxICON_INFORMATION, this);
                return;
            }
            if (!ExtendedAMSLite::connect_job_supported(printer.opt_string("printer_model"), filaments.size())) {
                wxMessageBox(_L("For more than four colors on an A1, prepare the print with Extended AMS Lite and use Studio's native LAN printing. Bambu Connect cannot preserve this workflow's slot assignments."),
                    _L("Bambu Connect"), wxOK | wxICON_INFORMATION, this);
                return;
            }
#ifdef _WIN32
            wxRegKey handler(wxRegKey::HKCR, "bambu-connect\\shell\\open\\command");
            if (!handler.Exists()) {
                if (wxMessageBox(_L("Install Bambu Connect to print with your Bambu account. Open the official download page?"),
                    _L("Bambu Connect"), wxYES_NO | wxICON_INFORMATION, this) == wxYES)
                    wxLaunchDefaultBrowser("https://wiki.bambulab.com/en/software/bambu-connect");
                return;
            }
#endif
            try {
                // Keep each exported job alive after returning to Studio. Connect
                // imports asynchronously; do not hand it a short-lived temp file.
                const auto folder = boost::filesystem::path(data_dir()) / "connect_jobs";
                boost::filesystem::create_directories(folder);
                const auto output = folder / boost::filesystem::unique_path("plate-%%%%-%%%%-%%%%.gcode.3mf");
                const auto strategy = SaveStrategy::Silence | SaveStrategy::SplitModel |
                    SaveStrategy::WithGcode | SaveStrategy::SkipModel;
                if (plater->export_3mf(output, strategy, plate->get_index()) < 0)
                    throw std::runtime_error("Could not export the sliced plate for Bambu Connect.");
                const std::string name = plater->get_export_gcode_filename("", true, false).utf8_string();
                const auto uri = ExtendedAMSLite::connect_import_uri(boost::filesystem::absolute(output).string(), name);
                if (!wxLaunchDefaultBrowser(wxString::FromUTF8(uri))) {
                    wxMessageBox(_L("Could not open Bambu Connect. You can import the saved sliced file manually:") +
                        "\n" + wxString::FromUTF8(output.string()), _L("Bambu Connect"), wxOK | wxICON_ERROR, this);
                }
            } catch (const std::exception& error) {
                wxMessageBox(wxString::FromUTF8(error.what()), _L("Bambu Connect"), wxOK | wxICON_ERROR, this);
            }
        });
    }
    auto* disable_swaps = new wxButton(this, wxID_ANY, _L("Disable spool swaps"));
    ams_row->Add(disable_swaps, 0, wxALL, 5);
    disable_swaps->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_ams_preparation.reset();
        const auto notes = wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_string("process_notes");
        set_ams_notes(ExtendedAMSLite::notes_with_execution(notes, std::nullopt));
        wxGetApp().plater()->reslice();
    });
    main_sizer->Add(ams_row, 0, wxEXPAND);
    swaps->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_gcode_result || m_gcode_result->moves.empty() ||
            (!m_only_gcode && !wxGetApp().plater()->get_partplate_list().get_curr_plate()->is_slice_result_valid())) {
            wxMessageBox(_L("Slice the current plate before calculating a swap schedule."), _L("Extended AMS Lite Mode"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        try {
            auto* plater = wxGetApp().plater();
            auto* plate = plater->get_partplate_list().get_curr_plate();
            const bool can_reorder = !m_only_gcode && wxGetApp().is_editor() &&
                plate->get_real_print_seq() == PrintSequence::ByLayer;
            const auto ready = ExtendedAMSLite::execution_from_notes(wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_string("process_notes"));
            ExtendedAMSLiteDialog dialog(this, *m_gcode_result, can_reorder,
                m_only_gcode ? std::vector<std::string>{} : wxGetApp().preset_bundle->filament_presets, ready ? &*ready : nullptr);
            const int action = dialog.ShowModal();
            if (action == wxID_APPLY || action == wxID_OK) {
                plater->take_snapshot("Prepare AMS Lite print");
                if (action == wxID_OK) {
                    const auto& printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
                    if (printer.opt_string("printer_model") != "Bambu Lab A1" && printer.opt_string("printer_model") != "A1")
                        throw std::runtime_error("Select the Bambu Lab A1 printer profile before preparing this print.");
                    m_ams_preparation = std::make_shared<AMSPreparation>();
                    m_ams_preparation->spec = dialog.proposal();
                    std::string id = boost::uuids::to_string(boost::uuids::random_generator()());
                    id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
                    m_ams_preparation->spec.job_id = id.substr(0, 16);
                    m_ams_preparation->plate = plate->get_index();
                }
                auto notes = ExtendedAMSLite::notes_with_execution(wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_string("process_notes"), std::nullopt);
                if (action == wxID_OK) notes += std::string("\n") + ExtendedAMSLite::execution_preparing_tag;
                set_ams_notes(notes);
                // Apply native per-layer sequences, then regenerate travel,
                // purge and tool changes through the normal slicing pipeline.
                // Never reorder already generated motion commands.
                std::vector<int> first;
                std::vector<LayerPrintSequence> others;
                for (const auto& layer : dialog.layer_orders()) {
                    std::vector<int> ids;
                    for (int id : layer.filaments) ids.push_back(id + 1);
                    if (layer.layer == 1) first = ids;
                    else if (!others.empty() && others.back().second == ids &&
                             others.back().first.second + 1 == int(layer.layer))
                        others.back().first.second = int(layer.layer);
                    else others.push_back({{int(layer.layer), int(layer.layer)}, std::move(ids)});
                }
                plate->set_first_layer_print_sequence(first);
                plate->set_other_layers_print_sequence(others);
                plater->update_project_dirty_from_presets();
                plater->set_plater_dirty(true);
                plater->config_change_notification(*plate->config(), "print_sequence");
                plater->update();
                plater->reslice();
            }
        } catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), _L("Extended AMS Lite Mode"), wxOK | wxICON_ERROR, this);
        }
    });
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    bind_event_handlers();

    if (wxGetApp().is_editor()) {
        m_ams_timer = std::make_unique<wxTimer>(this, wxWindow::NewControlId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { check_ams_pause(); }, m_ams_timer->GetId());
        m_ams_timer->Start(2000);
    }

    return true;
}

Preview::~Preview()
{
    if (m_ams_timer) m_ams_timer->Stop();
    unbind_event_handlers();

    if (m_canvas != nullptr)
        delete m_canvas;

    if (m_canvas_widget != nullptr)
        delete m_canvas_widget;
}

void Preview::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void Preview::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void Preview::select_view(const std::string& direction)
{
    m_canvas->select_view(direction);
}

void Preview::set_drop_target(wxDropTarget* target)
{
    if (target != nullptr)
        SetDropTarget(target);
}

//BBS: add only gcode mode
void Preview::load_print(bool keep_z_range, bool only_gcode)
{
    PrinterTechnology tech = m_process->current_printer_technology();
    if (tech == ptFFF)
        load_print_as_fff(keep_z_range, only_gcode);

    Layout();
    prepare_ams_slice();
}

void Preview::set_ams_notes(const std::string& notes)
{
    auto* bundle = wxGetApp().preset_bundle;
    bundle->prints.get_edited_preset().config.set_key_value("process_notes", new ConfigOptionString(notes));
    bundle->prints.update_dirty();
    auto* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    tab->reload_config();
    auto* plater = wxGetApp().plater();
    plater->on_config_change(bundle->full_config());
    plater->update_project_dirty_from_presets();
    plater->set_plater_dirty(true);
    plater->update();
}

void Preview::prepare_ams_slice()
{
    auto pending = m_ams_preparation;
    if (!pending || pending->scheduled || m_only_gcode || !m_gcode_result) return;
    auto* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (!plate || plate->get_index() != pending->plate || !plate->is_slice_result_ready_for_export()) return;
    pending->scheduled = true;
    // Run after the preview has finished loading; changing process notes here
    // invalidates G-code and must not re-enter the preview loader.
    CallAfter([this, pending] {
        if (m_ams_preparation != pending) return;
        try {
            auto* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
            if (!plate || plate->get_index() != pending->plate || !plate->is_slice_result_ready_for_export()) {
                pending->scheduled = false; return;
            }
            if (!pending->encoded) {
                auto requests = ExtendedAMSLite::collect_requests(m_gcode_result->moves, EMoveType::Tool_change, EMoveType::Extrude, true);
                if (m_gcode_result->filament_change_sequence.empty()) throw std::runtime_error("No native filament selections in this slice.");
                requests = ExtendedAMSLite::normalize_execution_requests(std::move(requests), int(m_gcode_result->filament_change_sequence.front()));
                pending->spec.plan = ExtendedAMSLite::plan_fixed_batches(requests, pending->spec.slots, pending->spec.plan.filament_slots);
                ExtendedAMSLite::verify_execution(pending->spec);
                pending->encoded = true;
                pending->scheduled = false;
                set_ams_notes(ExtendedAMSLite::notes_with_execution(wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_string("process_notes"), pending->spec));
                wxGetApp().plater()->reslice();
            } else {
                std::ifstream input(plate->get_slice_result()->filename, std::ios::binary);
                const auto error = ExtendedAMSLite::execution_gcode_error(input, pending->spec);
                if (!error.empty()) throw std::runtime_error(error);
                m_ams_preparation.reset();
                ExtendedAMSLiteDialog guide(this, *m_gcode_result, false, {}, &pending->spec);
                guide.ShowModal();
            }
        } catch (const std::exception& error) {
            m_ams_preparation.reset();
            wxMessageBox(wxString::FromUTF8(error.what()) + "\n\n" + _L("Use Disable spool swaps to return to an ordinary editable slice, then prepare the print again."),
                _L("AMS Lite preparation failed"), wxOK | wxICON_ERROR, this);
        }
    });
}

void Preview::check_ams_pause()
{
    auto* manager = wxGetApp().getDeviceManager();
    auto* machine = manager ? manager->get_selected_machine() : nullptr;
    if (!machine || !machine->is_connected() || !machine->is_info_ready()) return;
    const auto& name = machine->subtask_name;
    if (machine->print_status != "PAUSE" || name.rfind("EAMS-", 0) != 0 || name.size() < 21) {
        m_ams_paused_job.clear(); return;
    }
    const std::string id = name.substr(5, 16);
    if (!std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return;
    const std::string key = machine->get_dev_id() + ":" + id;
    if (key == m_ams_paused_job) return;
    try {
        const auto folder = boost::filesystem::path(data_dir()) / "extended_ams_jobs";
        boost::filesystem::ifstream target(folder / (id + ".target"), std::ios::binary);
        std::string serial; target >> serial;
        if (!target || serial != machine->get_dev_id()) return;
        boost::filesystem::ifstream input(folder / (id + ".plan"), std::ios::binary);
        if (!input || boost::filesystem::file_size(folder / (id + ".plan")) > 10000000) return;
        const auto spec = ExtendedAMSLite::deserialize_execution(std::string(std::istreambuf_iterator<char>(input), {}));
        if (spec.job_id != id) return;
        m_ams_paused_job = key;
        wxWeakRef<Preview> preview(this);
        wxGetApp().plater()->get_notification_manager()->push_notification(NotificationType::CustomNotification,
            NotificationManager::NotificationLevel::ImportantNotificationLevel,
            _u8L("Your Extended AMS Lite job reported a pause. Check the printer for errors or a manual pause before replacing spools. Choose the matching visit in the swap guide."),
            _u8L("Open swap guide"), [preview, spec](wxEvtHandler*) {
                if (!preview) return false;
                GCodeProcessorResult empty;
                ExtendedAMSLiteDialog guide(preview.get(), empty, false, {}, &spec);
                guide.ShowModal(); return true;
            });
    } catch (const std::exception&) {
        // Missing/invalid local job metadata must never trigger swap instructions.
    }
}

//BBS: add only gcode mode
void Preview::reload_print(bool keep_volumes, bool only_gcode)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: enter, keep_volumes %2%")%__LINE__ %keep_volumes;
#ifdef __linux__
    // We are getting mysterious crashes on Linux in gtk due to OpenGL context activation GH #1874 #1955.
    // So we are applying a workaround here: a delayed release of OpenGL vertex buffers.
    if (!IsShown())
    {
        m_volumes_cleanup_required = !keep_volumes;
        return;
    }
#endif /* __linux__ */
    if (
#ifdef __linux__
        m_volumes_cleanup_required ||
#endif /* __linux__ */
        !keep_volumes)
    {
        m_canvas->reset_volumes();
        //BBS: add m_loaded_print logic
        //m_loaded = false;
        m_loaded_print = nullptr;
#ifdef __linux__
        m_volumes_cleanup_required = false;
#endif /* __linux__ */
    }

    load_print(false, only_gcode);
    m_only_gcode = only_gcode;
}

//BBS: add only gcode mode
void Preview::refresh_print()
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: enter, current m_loaded_print %2%")%__LINE__ %m_loaded_print;
    //BBS: add m_loaded_print logic
    //m_loaded = false;
    m_loaded_print = nullptr;

    if (!IsShown())
        return;

    load_print(true, m_only_gcode);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: exit")%__LINE__;
}

//BBS: always load shell at preview
void Preview::load_shells(const Print& print, bool force_previewing)
{
    m_canvas->load_shells(print, force_previewing);
}

//BBS: always load shell at preview
void Preview::reset_shells()
{
    m_canvas->reset_shells();
}

void Preview::msw_rescale()
{
    // rescale warning legend on the canvas
    get_canvas3d()->msw_rescale();

    // rescale legend
    refresh_print();
}

void Preview::sys_color_changed()
{
    //TODO
    // m_layers_slider->sys_color_changed();
}

void Preview::on_tick_changed(CustomGCode::Type type)
{
    //if (type == Type::PausePrint) {
    //    m_schedule_background_process();
    //}
    m_keep_current_preview_type = false;
    reload_print(false);
}

void Preview::bind_event_handlers()
{
    this->Bind(wxEVT_SIZE, &Preview::on_size, this);
}

void Preview::unbind_event_handlers()
{
    this->Unbind(wxEVT_SIZE, &Preview::on_size, this);
}

void Preview::show_sliders(bool show)
{
    show_moves_sliders(show);
    show_layers_sliders(show);
}

void Preview::show_moves_sliders(bool show)
{
    ;//TODO
}

void Preview::show_layers_sliders(bool show)
{
    ;//TODO
}


void Preview::on_size(wxSizeEvent& evt)
{
    evt.Skip();
    Refresh();
}

// Find an index of a value in a sorted vector, which is in <z-eps, z+eps>.
// Returns -1 if there is no such member.
static int find_close_layer_idx(const std::vector<double> &zs, double &z, double eps)
{
    if (zs.empty()) return -1;
    auto it_h = std::lower_bound(zs.begin(), zs.end(), z);
    if (it_h == zs.end()) {
        auto it_l = it_h;
        --it_l;
        if (z - *it_l < eps) return int(zs.size() - 1);
    } else if (it_h == zs.begin()) {
        if (*it_h - z < eps) return 0;
    } else {
        auto it_l = it_h;
        --it_l;
        double dist_l = z - *it_l;
        double dist_h = *it_h - z;
        if (std::min(dist_l, dist_h) < eps) { return (dist_l < dist_h) ? int(it_l - zs.begin()) : int(it_h - zs.begin()); }
    }
    return -1;
}

void Preview::update_layers_slider_mode()
{
    //    true  -> single-extruder printer profile OR
    //             multi-extruder printer profile , but whole model is printed by only one extruder
    //    false -> multi-extruder printer profile , and model is printed by several extruders
    bool one_extruder_printed_model = true;
    bool can_change_color = true;
    // extruder used for whole model for multi-extruder printer profile
    int only_extruder = -1;

    // BBS
    if (wxGetApp().filaments_cnt() > 1) {
        //const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
        auto plate_extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_extruders_without_support();
        for (auto extruder : plate_extruders) {
            if (extruder != plate_extruders[0])
                can_change_color = false;
        }
        // check if whole model uses just only one extruder
        if (!plate_extruders.empty()) {
            //const int extruder = objects[0]->config.has("extruder") ? objects[0]->config.option("extruder")->getInt() : 0;
            only_extruder = plate_extruders[0];
            //    auto is_one_extruder_printed_model = [objects, extruder]() {
            //        for (ModelObject *object : objects) {
            //            if (object->config.has("extruder") && object->config.option("extruder")->getInt() != extruder) /*return false*/;

            //            for (ModelVolume *volume : object->volumes)
            //                if ((volume->config.has("extruder") && volume->config.option("extruder")->getInt() != extruder) || !volume->mmu_segmentation_facets.empty()) return false;

            //            for (const auto &range : object->layer_config_ranges)
            //                if (range.second.has("extruder") && range.second.option("extruder")->getInt() != extruder) return false;
            //        }
            //        return true;
            //    };

            //    if (is_one_extruder_printed_model())
            //        only_extruder = extruder;
            //    else
            //        one_extruder_printed_model = false;
        }
    }

    IMSlider *m_layers_slider = m_canvas->get_gcode_viewer().get_layers_slider();
    m_layers_slider->SetModeAndOnlyExtruder(one_extruder_printed_model, only_extruder, can_change_color);
}

void Preview::update_layers_slider_from_canvas(wxKeyEvent &event)
{
    if (event.HasModifiers()) {
        event.Skip();
        return;
    }

    const auto key = event.GetKeyCode();

    IMSlider *m_layers_slider = m_canvas->get_gcode_viewer().get_layers_slider();
    IMSlider *m_moves_slider  = m_canvas->get_gcode_viewer().get_moves_slider();
    if (key == 'L') {
        if(!m_layers_slider->switch_one_layer_mode())
            event.Skip();
        m_canvas->set_as_dirty();
    }
    /*else if (key == WXK_SHIFT)
        m_layers_slider->UseDefaultColors(false);*/
    else
        event.Skip();
}

void Preview::update_layers_slider(const std::vector<double>& layers_z, bool keep_z_range)
{
    IMSlider *m_layers_slider = m_canvas->get_gcode_viewer().get_layers_slider();
    // Save the initial slider span.
    double z_low     = m_layers_slider->GetLowerValueD();
    double z_high    = m_layers_slider->GetHigherValueD();
    bool   was_empty = m_layers_slider->GetMaxValue() == 0;

    bool force_sliders_full_range = was_empty;
    if (!keep_z_range) {
        bool span_changed = layers_z.empty() || std::abs(layers_z.back() - m_layers_slider->GetMaxValueD()) > epsilon() /*1e-6*/;
        force_sliders_full_range |= span_changed;
    }
    bool snap_to_min = force_sliders_full_range || m_layers_slider->is_lower_at_min();
    bool snap_to_max = force_sliders_full_range || m_layers_slider->is_higher_at_max();

    // Detect and set manipulation mode for double slider
    update_layers_slider_mode();

    Plater* plater = wxGetApp().plater();
    //BBS: replace model custom gcode with current plate custom gcode
    CustomGCode::Info ticks_info_from_curr_plate;
    if (wxGetApp().is_editor())
        ticks_info_from_curr_plate = plater->model().get_curr_plate_custom_gcodes();
    else {
        ticks_info_from_curr_plate.mode   = CustomGCode::Mode::SingleExtruder;
        ticks_info_from_curr_plate.gcodes = m_canvas->get_custom_gcode_per_print_z();
    }
    // first of all update extruder colors to avoid crash, when we are switching printer preset from MM to SM
    m_layers_slider->SetExtruderColors(plater->get_extruder_colors_from_plater_config(wxGetApp().is_editor() ? nullptr : m_gcode_result));
    m_layers_slider->SetSliderValues(layers_z);
    assert(m_layers_slider->GetMinValue() == 0);
    m_layers_slider->SetMaxValue(layers_z.empty() ? 0 : layers_z.size() - 1);

    int idx_low  = 0;
    int idx_high = m_layers_slider->GetMaxValue();
    if (!layers_z.empty()) {
        if (!snap_to_min) {
            int idx_new = find_close_layer_idx(layers_z, z_low, epsilon() /*1e-6*/);
            if (idx_new != -1) idx_low = idx_new;
        }
        if (!snap_to_max) {
            int idx_new = find_close_layer_idx(layers_z, z_high, epsilon() /*1e-6*/);
            if (idx_new != -1) idx_high = idx_new;
        }
    }
    m_layers_slider->SetSelectionSpan(idx_low, idx_high);

    auto curr_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    auto curr_print_seq = curr_plate->get_real_print_seq();
    bool sequential_print = (curr_print_seq == PrintSequence::ByObject);
    m_layers_slider->SetDrawMode(sequential_print);

    m_layers_slider->SetTicksValues(ticks_info_from_curr_plate);

    auto print_mode_stat = m_gcode_result->print_statistics.modes.front();
    m_layers_slider->SetLayersTimes(print_mode_stat.layers_times, print_mode_stat.time);

    // Suggest the auto color change, if model looks like sign
    if (m_layers_slider->IsNewPrint()) {
        const Print &print = wxGetApp().plater()->fff_print();

        // bool is_possible_auto_color_change = false;
        for (auto object : print.objects()) {
            double object_x = double(object->size().x());
            double object_y = double(object->size().y());

            // if it's sign, than object have not to be a too height
            double  height      = object->height();
            coord_t longer_side = std::max(object_x, object_y);
            auto    num_layers  = int(object->layers().size());
            if (height / longer_side > 0.3 || num_layers < 2) continue;

            const ExPolygons &bottom      = object->get_layer(0)->lslices;
            double            bottom_area = area(bottom);

            // at least 25% of object's height have to be a solid
            int i, min_solid_height = int(0.25 * num_layers);
            for (i = 1; i <= min_solid_height; ++i) {
                double cur_area = area(object->get_layer(i)->lslices);
                if (!equivalent_areas(bottom_area, cur_area)) {
                    // but due to the elephant foot compensation, the first layer may be slightly smaller than the others
                    if (i == 1 && fabs(cur_area - bottom_area) / bottom_area < 0.1) {
                        // So, let process this case and use second layer as a bottom
                        bottom_area = cur_area;
                        continue;
                    }
                    break;
                }
            }
            if (i < min_solid_height) continue;
        }
    }
}

//BBS: add only gcode mode
void Preview::load_print_as_fff(bool keep_z_range, bool only_gcode)
{
    if (wxGetApp().mainframe == nullptr || wxGetApp().is_recreating_gui())
        // avoid processing while mainframe is being constructed
        return;

    //BBS: add m_loaded_print logic
    const Print *print = m_process->fff_print();
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: previous print %2%, new print %3%")%__LINE__ %m_loaded_print %print;
    if ((m_loaded_print&&(m_loaded_print == print)) || m_process->current_printer_technology() != ptFFF) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: already loaded before, return directly")%__LINE__;
        return;
    }

    // we require that there's at least one object and the posSlice step
    // is performed on all of them(this ensures that _shifted_copies was
    // populated and we know the number of layers)
    bool has_layers = false;
    //BBS: always load shell at preview
    load_shells(*print, true);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: print: %2%, gcode_result %3%, check started")%__LINE__ %print %m_gcode_result;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: print is step done, posSlice %2%, posSupportMaterial %3%, psGCodeExport %4%") % __LINE__ % print->is_step_done(posSlice) %print->is_step_done(posSupportMaterial) % print->is_step_done(psGCodeExport);
    if (print->is_step_done(posSlice)) {
        for (const PrintObject* print_object : print->objects())
            if (! print_object->layers().empty()) {
                has_layers = true;
                break;
            }
    }
    if (print->is_step_done(posSupportMaterial)) {
        for (const PrintObject* print_object : print->objects())
            if (! print_object->support_layers().empty()) {
                has_layers = true;
                break;
            }
    }

    //BBS: support preview gcode directly even if no slicing
    bool directly_preview = (only_gcode || print->is_step_done(psGCodeExport)) && !m_gcode_result->moves.empty();
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": directly_preview: %1%, gcode_result moves %2%, has_layers %3%") % directly_preview % m_gcode_result->moves.size() %has_layers;
    if (wxGetApp().is_editor() && !has_layers && !directly_preview) {
        show_sliders(false);
        m_canvas_widget->Refresh();
        return;
    }
    //BBS: for direct preview, don't keep z range
    else if (directly_preview && !has_layers)
        keep_z_range = false;

    const auto& t_gcode_viewer = m_canvas->get_gcode_viewer();
    gcode::EViewType gcode_view_type = t_gcode_viewer.get_view_type();
    bool gcode_preview_data_valid = !m_gcode_result->moves.empty();

    // Collect colors per extruder.
    std::vector<std::string> colors;
    std::vector<CustomGCode::Item> color_print_values = {};
    // set color print values, if it si selected "ColorPrint" view type
    if (gcode_view_type == gcode::EViewType::ColorPrint) {
        colors = wxGetApp().plater()->get_colors_for_color_print(m_gcode_result);

        if (!gcode_preview_data_valid) {
            if (wxGetApp().is_editor())
                //BBS
                color_print_values = wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes;
            else
                color_print_values = m_canvas->get_custom_gcode_per_print_z();
            colors.push_back("#808080"); // gray color for pause print or custom G-code
        }
    }
    else if (gcode_preview_data_valid || gcode_view_type == gcode::EViewType::Tool) {
        colors = wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result);
        color_print_values.clear();
    }

    std::vector<double> zs;

    if (IsShown()) {
        m_canvas->set_selected_extruder(0);
        bool is_slice_result_valid = wxGetApp().plater()->get_partplate_list().get_curr_plate()->is_slice_result_valid();
        if (gcode_preview_data_valid && (is_slice_result_valid || only_gcode)) {
            // Load the real G-code preview.
            //BBS: add more log
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": will load gcode_preview from result, moves count %1%") % m_gcode_result->moves.size();
            //BBS: add only gcode mode
            m_canvas->load_gcode_preview(*m_gcode_result, colors, only_gcode);
            //BBS show sliders
            show_moves_sliders();

            //BBS: keep shell preview on or not by app_config
            m_canvas->set_shells_on_previewing(wxGetApp().app_config->get_bool("show_shells_in_preview"));
            Refresh();
            zs = m_canvas->get_gcode_layers_zs();
            //BBS: add m_loaded_print logic
            //m_loaded = true;
            m_loaded_print = print;
        }
        else if (wxGetApp().is_editor()) {
            // Load the initial preview based on slices, not the final G-code.
            //BBS: only display shell before slicing result out
            //m_canvas->load_preview(colors, color_print_values);
            show_moves_sliders(false);
            Refresh();
            //zs = m_canvas->get_volumes_print_zs(true);
        }

        if (!zs.empty() && !m_keep_current_preview_type) {
            unsigned int number_extruders = wxGetApp().is_editor() ?
                (unsigned int)print->extruders().size() :
                m_canvas->get_gcode_extruders_count();
            std::vector<CustomGCode::Item> gcodes = wxGetApp().is_editor() ?
                //BBS
                wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes :
                m_canvas->get_custom_gcode_per_print_z();
            const wxString choice = !gcodes.empty() ?
                _L("Multicolor Print") :
                (number_extruders > 1) ? _L("Filaments") : _L("Line Type");
        }

        if (zs.empty()) {
            // all layers filtered out
            //BBS
            show_layers_sliders(false);
            m_canvas_widget->Refresh();
        } else
            update_layers_slider(zs, keep_z_range);
    }
}

AssembleView::AssembleView(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
    : BaseView()
{
    init(parent, bed, model, config, process);
    m_p_override_camera = std::make_shared<Camera>();
    m_p_override_camera->enable_update_config_on_type_change(false);
}

AssembleView::~AssembleView()
{
    delete m_canvas;
    delete m_canvas_widget;
    m_p_override_camera = nullptr;
}

bool AssembleView::init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return false;
    }
    const GUI::EMSAAType msaa_type = p_ogl_manager->is_fxaa_enabled() ? GUI::EMSAAType::Disabled : p_ogl_manager->get_msaa_type();
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, msaa_type);
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));

    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    // XXX: If have OpenGL
    m_canvas->enable_picking(true);
    m_canvas->enable_moving(true);
    // XXX: more config from 3D.pm
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasAssembleView);
    m_canvas->set_config(config);
    m_canvas->enable_gizmos(true);
    m_canvas->enable_selection(true);
    m_canvas->enable_main_toolbar(true);
    m_canvas->enable_labels(false);
    m_canvas->enable_slope(false);
    //BBS: GUI refactor: GLToolbar
    m_canvas->enable_return_toolbar(true);
    // BBS: set volume_selection_mode to Volume
    //same to 3d //m_canvas->get_selection().set_volume_selection_mode(Selection::Instance);
    //m_canvas->get_selection().lock_volume_selection_mode();

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    return true;
}

void AssembleView::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void AssembleView::render()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

bool AssembleView::is_reload_delayed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_reload_delayed() : false;
}

void AssembleView::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas != nullptr) {
        if (!m_canvas->is_initialized()) {
            m_canvas->render(true);
        }
        m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
    }
}

void AssembleView::select_view(const std::string& direction)
{
    if (m_canvas != nullptr)
        m_canvas->select_view(direction);
}

} // namespace GUI
} // namespace Slic3r

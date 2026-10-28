#ifndef slic3r_GUI_ExtendedAMSLiteDialog_hpp_
#define slic3r_GUI_ExtendedAMSLiteDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/ExtendedAMSLite/Planner.hpp"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxFlexGridSizer;
class wxListCtrl;
class wxScrolledWindow;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
struct GCodeProcessorResult;
namespace GUI {

class ExtendedAMSLiteDialog : public DPIDialog
{
public:
    ExtendedAMSLiteDialog(wxWindow* parent, const GCodeProcessorResult& result);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    std::vector<ExtendedAMSLite::Filament> m_filaments;
    std::vector<ExtendedAMSLite::Request> m_requests;
    wxSpinCtrl* m_slot_count;
    wxScrolledWindow* m_slot_panel;
    wxFlexGridSizer* m_slot_sizer;
    std::vector<wxChoice*> m_initial;
    std::vector<wxCheckBox*> m_locked;
    wxStaticText* m_summary;
    wxTextCtrl* m_initial_text;
    wxListCtrl* m_schedule;
    wxButton* m_export;
    std::string m_export_text;
    std::string m_export_html;

    void rebuild_slots();
    void invalidate();
    void calculate();
    void export_schedule();
    wxString filament_label(int id) const;
};

}} // namespace Slic3r::GUI
#endif

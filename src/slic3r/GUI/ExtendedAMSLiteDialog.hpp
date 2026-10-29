#ifndef slic3r_GUI_ExtendedAMSLiteDialog_hpp_
#define slic3r_GUI_ExtendedAMSLiteDialog_hpp_
#include "GUI_Utils.hpp"
#include "libslic3r/ExtendedAMSLite/Execution.hpp"
#include <array>
class wxButton;
class wxCheckBox;
class wxChoice;
class wxListBox;
class wxPanel;
class wxStaticText;
namespace Slic3r {
struct GCodeProcessorResult;
namespace GUI {
class ExtendedAMSLiteDialog : public DPIDialog {
public:
    ExtendedAMSLiteDialog(wxWindow*, const GCodeProcessorResult&, bool can_reorder,
                          const std::vector<std::string>& profile_names = {},
                          const ExtendedAMSLite::ExecutionSpec* ready = nullptr);
    std::vector<ExtendedAMSLite::LayerOrder> layer_orders() const { return ExtendedAMSLite::complete_layer_orders(m_plan); }
    ExtendedAMSLite::ExecutionSpec proposal() const;
protected:
    void on_dpi_changed(const wxRect&) override;
private:
    struct Card { wxPanel* color; wxStaticText* text; };
    std::vector<ExtendedAMSLite::Filament> m_filaments;
    std::vector<ExtendedAMSLite::Request> m_requests;
    ExtendedAMSLite::BatchPlan m_plan;
    std::array<Card, 4> m_loading, m_changes;
    std::array<wxChoice*, 4> m_initial;
    std::array<wxCheckBox*, 4> m_locked;
    wxCheckBox *m_optimize, *m_manual;
    wxListBox* m_order;
    std::vector<int> m_order_ids;
    wxStaticText *m_summary, *m_detail;
    wxChoice* m_visits;
    wxButton *m_apply, *m_export, *m_prepare;
    bool m_ready = false;
    bool m_valid = false, m_can_reorder = false;
    void calculate();
    void show_visit();
    void move_color(int);
    void export_schedule();
    wxString filament_label(int) const;
    Card make_card(wxSizer*, const wxString&);
    void set_card(Card&, int, const wxString&);
};
}}
#endif

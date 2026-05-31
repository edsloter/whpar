// whpar - High-Speed Fountain Parity GUI Tool
// Copyright (C) 2026 Edward Sloter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <wx/wx.h>
#include <wx/notebook.h>
#include "resource.h"
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/statbox.h>
#include <wx/spinctrl.h>
#include <wx/checkbox.h>
#include <wx/gauge.h>
#include <wx/filepicker.h>

#include <wx/choice.h>
#include <wx/thread.h>
#include <wx/msgdlg.h>
#include <wx/tokenzr.h>
#include <wx/dnd.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
#include "wirehair.h"
#include <sstream>
#include <streambuf>
#include <iostream>
#include <thread>
#include <atomic>

#include "parity.h"
#include "create.h"
#include "repair.h"

// ── Stream redirector: captures cout/cerr into a wxTextCtrl ──
class TextCtrlStream : public std::streambuf {
    wxTextCtrl* m_text;
    std::string m_buf;
public:
    TextCtrlStream(wxTextCtrl* tc) : m_text(tc) {}
protected:
    int overflow(int c) override {
        if (c == '\n') {
            flush();
            m_text->AppendText("\n");
        } else {
            m_buf += static_cast<char>(c);
        }
        return c;
    }
    int sync() override { flush(); return 0; }
private:
    void flush() {
        if (!m_buf.empty()) {
            wxString s(m_buf);
            m_text->AppendText(s);
            m_buf.clear();
        }
    }
};

// ── Worker thread ──
enum class WorkMode { Create, Repair, Add, Info, List };

struct WorkParams {
    WorkMode mode;
    std::vector<std::string> sourcePaths;
    std::string parityPath;
    std::string outputPath;
    float overhead = 0.10f;
    bool debug = false;
    bool force = false;
    bool useXxh64 = false;
    bool noRecursive = false;
    bool showTiming = false;
    uint32_t blockSizeKB = 0;
    uint32_t numJobs = 0;
    uint64_t maxMemBytes = 0;
};

// ── Drop target for file drag-and-drop ──
class CreateSrcDropTarget : public wxFileDropTarget {
public:
    CreateSrcDropTarget(wxTextCtrl* tc) : m_tc(tc) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override {
        wxString existing = m_tc->GetValue();
        for (size_t i = 0; i < filenames.size(); ++i) {
            if (!existing.empty() && !existing.EndsWith("\n"))
                existing += "\n";
            existing += filenames[i];
        }
        m_tc->SetValue(existing);
        return true;
    }
private:
    wxTextCtrl* m_tc;
};

// ── Main frame ──
class WhparFrame : public wxFrame {
public:
    WhparFrame() : wxFrame(nullptr, wxID_ANY, "whpar " WHPAR_VERSION " - High-Speed Fountain Parity Tool",
                           wxDefaultPosition, wxSize(900, 700)) {
        auto* panel = new wxPanel(this);
        auto* topSizer = new wxBoxSizer(wxVERTICAL);

        // ── Notebook tabs ──
        m_notebook = new wxNotebook(panel, wxID_ANY);

        CreateCreateTab();
        CreateRepairTab();
        CreateAddTab();
        CreateInfoTab();
        CreateListTab();
        CreateSettingsTab();
        CreateAboutTab();

        topSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 5);

        // ── Output log ──
        auto* logBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Output");
        m_output = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxSize(-1, 200),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        logBox->Add(m_output, 1, wxEXPAND | wxALL, 5);
        topSizer->Add(logBox, 0, wxEXPAND | wxALL, 5);

        // ── Progress gauge ──
        m_progressGauge = new wxGauge(panel, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize,
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        topSizer->Add(m_progressGauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

        // ── Bottom buttons ──
        auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
        m_runBtn = new wxButton(panel, wxID_ANY, "Run");
        m_runBtn->Bind(wxEVT_BUTTON, &WhparFrame::OnRun, this);
        auto* clearBtn = new wxButton(panel, wxID_ANY, "Clear Output");
        clearBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_output->Clear(); });
        auto* quitBtn = new wxButton(panel, wxID_EXIT, "Quit");
        quitBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

        btnSizer->Add(m_runBtn, 0, wxRIGHT, 5);
        btnSizer->Add(clearBtn, 0, wxRIGHT, 5);
        btnSizer->AddStretchSpacer();
        btnSizer->Add(quitBtn, 0, wxRIGHT, 5);
        topSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 5);

        panel->SetSizer(topSizer);
        CreateStatusBar();
        SetStatusText("Ready");
#ifdef _WIN32
        {
            HICON hIcon = ::LoadIconW(
                ::GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_ICON1)
            );
            if (hIcon) {
                ::SendMessageW((HWND)GetHWND(), WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
                ::SendMessageW((HWND)GetHWND(), WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }
        }
#endif

        // Redirect cout/cerr
        m_coutBuf = std::cout.rdbuf();
        m_cerrBuf = std::cerr.rdbuf();
        m_stream = new TextCtrlStream(m_output);
        std::cout.rdbuf(m_stream);
        std::cerr.rdbuf(m_stream);
    }

    ~WhparFrame() override {
        if (m_worker.joinable()) {
            m_worker.detach();
        }
        std::cout.rdbuf(m_coutBuf);
        std::cerr.rdbuf(m_cerrBuf);
        delete m_stream;
    }

    void LoadArchive(const wxString& path) {
        m_infoArchive->SetPath(path);
        m_notebook->SetSelection(3);
        wxCommandEvent dummy;
        OnRun(dummy);
    }

private:
    wxNotebook* m_notebook;

    // Create tab
    wxPanel* m_createPanel;
    wxTextCtrl* m_createSources;
    wxFilePickerCtrl* m_createOutFile;
    wxSpinCtrlDouble* m_createOverhead;
    wxSpinCtrl* m_createJobs;
    wxSpinCtrl* m_createBlockSize;
    wxCheckBox* m_createXxh64;
    wxCheckBox* m_createNoRecursive;
    wxCheckBox* m_createDebug;
    wxCheckBox* m_createForce;

    // Repair tab
    wxPanel* m_repairPanel;
    wxFilePickerCtrl* m_repairArchive;
    wxDirPickerCtrl* m_repairOutDir;
    wxFilePickerCtrl* m_repairOutFile;
    wxCheckBox* m_repairOutIsFile;
    wxFilePickerCtrl* m_repairDamaged;
    wxStaticText* m_repairDamagedLabel;
    wxSpinCtrl* m_repairJobs;
    wxCheckBox* m_repairForce;
    wxCheckBox* m_repairDebug;
    wxCheckBox* m_repairTiming;
    wxTextCtrl* m_repairMaxMem;

    // Add tab
    wxPanel* m_addPanel;
    wxFilePickerCtrl* m_addArchive;
    wxSpinCtrlDouble* m_addOverhead;
    wxSpinCtrl* m_addJobs;
    wxCheckBox* m_addForce;
    wxCheckBox* m_addDebug;

    // Info tab
    wxPanel* m_infoPanel;
    wxFilePickerCtrl* m_infoArchive;
    wxDirPickerCtrl* m_infoSourceDir;
    wxCheckBox* m_infoDebug;

    // List tab
    wxPanel* m_listPanel;
    wxFilePickerCtrl* m_listArchive;

    // Settings tab
    wxPanel* m_settingsPanel;
    wxCheckBox* m_settingsAssoc;
    wxStaticText* m_settingsStatus;

    // About tab
    wxPanel* m_aboutPanel;

    wxTextCtrl* m_output;
    wxGauge* m_progressGauge;
    wxButton* m_runBtn;

    std::thread m_worker;
    std::atomic<bool> m_running{false};

    TextCtrlStream* m_stream = nullptr;
    std::streambuf* m_coutBuf = nullptr;
    std::streambuf* m_cerrBuf = nullptr;

    WorkParams CollectParams() {
        WorkParams p;
        int sel = m_notebook->GetSelection();

        if (sel == 0) { // Create
            p.mode = WorkMode::Create;
            wxString src = m_createSources->GetValue();
            wxStringTokenizer tk(src, "\n");
            while (tk.HasMoreTokens()) {
                wxString s = tk.GetNextToken().Trim();
                if (!s.empty()) {
                    if (s.Length() > 1 && s[0] == '"' && s[s.Length()-1] == '"')
                        s = s.SubString(1, s.Length()-2);
                    if (!s.empty())
                        p.sourcePaths.push_back(s.ToStdString());
                }
            }
            p.parityPath = m_createOutFile->GetPath().ToStdString();
            if (p.parityPath.empty() && !p.sourcePaths.empty()) {
                size_t lastSep = p.sourcePaths[0].find_last_of("/\\");
                std::string baseName = (lastSep == std::string::npos)
                    ? p.sourcePaths[0] : p.sourcePaths[0].substr(lastSep + 1);
                std::string sourceDir = (lastSep == std::string::npos)
                    ? "" : p.sourcePaths[0].substr(0, lastSep + 1);
                size_t dot = baseName.find_last_of('.');
                if (dot != std::string::npos)
                    baseName = baseName.substr(0, dot);
                int pct = static_cast<int>(m_createOverhead->GetValue() * 100.0f + 0.5f);
                if (pct < 1) pct = 1;
                if (pct > 99) pct = 99;
                std::string pStr = (pct < 10 ? "0" : "") + std::to_string(pct);
                p.parityPath = sourceDir + baseName + ".p" + pStr + ".whpar";
                std::cout << "No output specified. Using '" << p.parityPath << "'.\n";
            }
            p.overhead = static_cast<float>(m_createOverhead->GetValue());
            p.numJobs = static_cast<uint32_t>(m_createJobs->GetValue());
            p.blockSizeKB = static_cast<uint32_t>(m_createBlockSize->GetValue());
            p.useXxh64 = m_createXxh64->GetValue();
            p.noRecursive = m_createNoRecursive->GetValue();
            p.debug = m_createDebug->GetValue();
            p.force = m_createForce->GetValue();
        } else if (sel == 1) { // Repair
            p.mode = WorkMode::Repair;
            p.parityPath = m_repairArchive->GetPath().ToStdString();
            if (m_repairOutIsFile->GetValue()) {
                p.outputPath = m_repairOutFile->GetPath().ToStdString();
                p.sourcePaths.push_back(!m_repairDamaged->GetPath().empty()
                    ? m_repairDamaged->GetPath().ToStdString()
                    : p.outputPath);
            } else {
                p.outputPath = m_repairOutDir->GetPath().ToStdString();
            }
            p.numJobs = static_cast<uint32_t>(m_repairJobs->GetValue());
            p.force = m_repairForce->GetValue();
            p.debug = m_repairDebug->GetValue();
            p.showTiming = m_repairTiming->GetValue();
            wxString mem = m_repairMaxMem->GetValue();
            if (!mem.empty()) {
                try {
                    std::string v = mem.ToStdString();
                    size_t pos = 0;
                    while (pos < v.size() && (v[pos] == '.' || (v[pos] >= '0' && v[pos] <= '9'))) pos++;
                    if (pos > 0) {
                        double num = std::stod(v.substr(0, pos));
                        std::string suf;
                        for (size_t i = pos; i < v.size(); i++) suf += static_cast<char>(toupper(v[i]));
                        if (suf == "G" || suf == "GB" || suf == "GIB")
                            p.maxMemBytes = static_cast<uint64_t>(num * 1024ULL * 1024ULL * 1024ULL);
                        else if (suf == "M" || suf == "MB" || suf == "MIB")
                            p.maxMemBytes = static_cast<uint64_t>(num * 1024ULL * 1024ULL);
                        else if (suf == "K" || suf == "KB" || suf == "KIB")
                            p.maxMemBytes = static_cast<uint64_t>(num * 1024ULL);
                        else
                            p.maxMemBytes = static_cast<uint64_t>(num);
                    }
                } catch (...) {}
            }
        } else if (sel == 2) { // Add
            p.mode = WorkMode::Add;
            p.parityPath = m_addArchive->GetPath().ToStdString();
            p.overhead = static_cast<float>(m_addOverhead->GetValue());
            p.numJobs = static_cast<uint32_t>(m_addJobs->GetValue());
            p.force = m_addForce->GetValue();
            p.debug = m_addDebug->GetValue();
        } else if (sel == 3) { // Info
            p.mode = WorkMode::Info;
            p.parityPath = m_infoArchive->GetPath().ToStdString();
            p.outputPath = m_infoSourceDir->GetPath().ToStdString();
            p.debug = m_infoDebug->GetValue();
        } else if (sel == 4) { // List
            p.mode = WorkMode::List;
            p.parityPath = m_listArchive->GetPath().ToStdString();
        }

        return p;
    }

    void OnRun(wxCommandEvent&) {
        if (m_running) {
            wxMessageBox("Operation already in progress.", "Busy", wxOK | wxICON_INFORMATION);
            return;
        }
        m_running = true;
        m_runBtn->Disable();
        m_output->Clear();
        m_progressGauge->SetValue(0);
        SetStatusText("Running...");

        WorkParams params = CollectParams();
        g_progressCallback = [this](int pct) {
            CallAfter([this, pct]() { m_progressGauge->SetValue(pct); });
        };
        m_worker = std::thread([this, params]() {
            if (wirehair_init() != Wirehair_Success) {
                std::cerr << "CRITICAL: Wirehair initialization failed!\n";
            } else {
                switch (params.mode) {
                    case WorkMode::Create:
                        CreateParity(params.sourcePaths, params.parityPath, params.overhead,
                                     params.debug, params.blockSizeKB, params.useXxh64,
                                     params.numJobs, params.noRecursive, params.maxMemBytes);
                        break;
                    case WorkMode::Repair: {
                        std::string damaged = params.sourcePaths.empty() ? "" : params.sourcePaths[0];
                        std::string out = params.outputPath.empty() ? "." : params.outputPath;
                        RepairDataset(damaged, params.parityPath, out,
                                      params.force, params.debug, params.showTiming,
                                      params.numJobs, params.maxMemBytes);
                        break;
                    }
                    case WorkMode::Add:
                        AddParity(params.parityPath, params.overhead, params.debug,
                                  params.force, params.numJobs, params.noRecursive,
                                  params.maxMemBytes);
                        break;
                    case WorkMode::Info:
                        InfoCheck(params.parityPath, params.debug, params.outputPath);
                        break;
                    case WorkMode::List:
                        ListManifest(params.parityPath);
                        break;
                }

            }

            CallAfter([this]() {
                m_running = false;
                m_progressGauge->SetValue(100);
                g_progressCallback = nullptr;
                m_runBtn->Enable();
                SetStatusText("Done");
            });
        });
    }

    // ── Tab builders ──
    void CreateCreateTab() {
        m_createPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);

        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_createPanel, wxID_ANY, "Sources (one per line)\nDrag/Drop supported."));
        auto* srcSizer = new wxBoxSizer(wxHORIZONTAL);
        m_createSources = new wxTextCtrl(m_createPanel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 120), wxTE_MULTILINE);
        m_createSources->SetDropTarget(new CreateSrcDropTarget(m_createSources));
        srcSizer->Add(m_createSources, 1, wxEXPAND);
        auto* btnSizer = new wxBoxSizer(wxVERTICAL);
        auto* addFilesBtn = new wxButton(m_createPanel, wxID_ANY, "Add Files");
        auto* addDirBtn = new wxButton(m_createPanel, wxID_ANY, "Add Folder");
        btnSizer->Add(addFilesBtn, 0, wxBOTTOM, 5);
        btnSizer->Add(addDirBtn);
        srcSizer->Add(btnSizer, 0, wxLEFT, 5);
        gb->Add(srcSizer, 1, wxEXPAND);

        addFilesBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxFileDialog dlg(m_createPanel, "Select files", "", "",
                             wxFileSelectorDefaultWildcardStr,
                             wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK) {
                wxArrayString paths;
                dlg.GetPaths(paths);
                wxString existing = m_createSources->GetValue();
                for (size_t i = 0; i < paths.size(); ++i) {
                    if (!existing.empty() && !existing.EndsWith("\n"))
                        existing += "\n";
                    existing += paths[i];
                }
                m_createSources->SetValue(existing);
            }
        });
        addDirBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxDirDialog dlg(m_createPanel, "Select directory", "",
                            wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK) {
                wxString dir = dlg.GetPath();
                wxString existing = m_createSources->GetValue();
                if (!existing.empty() && !existing.EndsWith("\n"))
                    existing += "\n";
                existing += dir;
                m_createSources->SetValue(existing);
            }
        });

        gb->Add(new wxStaticText(m_createPanel, wxID_ANY, "Output .whpar (optional, default = source):"));
        m_createOutFile = new wxFilePickerCtrl(m_createPanel, wxID_ANY, "", "Save to...", "*.whpar",
                                               wxDefaultPosition, wxDefaultSize,
                                               wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
        gb->Add(m_createOutFile, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_createPanel, wxID_ANY, "Overhead (e.g. 0.10 = 10%):"));
        m_createOverhead = new wxSpinCtrlDouble(m_createPanel, wxID_ANY, "0.10", wxDefaultPosition, wxDefaultSize,
                                                 wxSP_ARROW_KEYS, 0.01, 1.0, 0.10, 0.01);
        gb->Add(m_createOverhead, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_createPanel, wxID_ANY, "Jobs (0 = auto):"));
        m_createJobs = new wxSpinCtrl(m_createPanel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                      wxSP_ARROW_KEYS, 0, 128, 0);
        gb->Add(m_createJobs, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_createPanel, wxID_ANY, "Block size KB (0 = auto):"));
        m_createBlockSize = new wxSpinCtrl(m_createPanel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                           wxSP_ARROW_KEYS, 0, 1048576, 0);
        gb->Add(m_createBlockSize, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_createPanel, wxID_ANY, ""));
        auto* cbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_createXxh64 = new wxCheckBox(m_createPanel, wxID_ANY, "XXH64");
        m_createNoRecursive = new wxCheckBox(m_createPanel, wxID_ANY, "No Recursive");
        m_createDebug = new wxCheckBox(m_createPanel, wxID_ANY, "Debug");
        m_createForce = new wxCheckBox(m_createPanel, wxID_ANY, "Force");
        cbSizer->Add(m_createXxh64, 0, wxRIGHT, 10);
        cbSizer->Add(m_createNoRecursive, 0, wxRIGHT, 10);
        cbSizer->Add(m_createDebug, 0, wxRIGHT, 10);
        cbSizer->Add(m_createForce);
        gb->Add(cbSizer);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_createPanel->SetSizer(s);
        m_notebook->AddPage(m_createPanel, "Create");
    }

    void CreateRepairTab() {
        m_repairPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_repairPanel, wxID_ANY, "Archive (.whpar):"));
        m_repairArchive = new wxFilePickerCtrl(m_repairPanel, wxID_ANY, "", "Select parity archive",
                                               "*.whpar", wxDefaultPosition, wxDefaultSize,
                                               wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        gb->Add(m_repairArchive, 1, wxEXPAND);

        m_repairDamagedLabel = new wxStaticText(m_repairPanel, wxID_ANY, "Damaged file (optional):");
        gb->Add(m_repairDamagedLabel);
        m_repairDamaged = new wxFilePickerCtrl(m_repairPanel, wxID_ANY, "", "Select damaged file",
                                               wxFileSelectorDefaultWildcardStr,
                                               wxDefaultPosition, wxDefaultSize,
                                               wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        gb->Add(m_repairDamaged, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_repairPanel, wxID_ANY, "Output (optional):"));
        auto* outSizer = new wxBoxSizer(wxHORIZONTAL);
        m_repairOutDir = new wxDirPickerCtrl(m_repairPanel, wxID_ANY, "", "Select output directory",
                                             wxDefaultPosition, wxDefaultSize,
                                             wxDIRP_USE_TEXTCTRL);
        m_repairOutFile = new wxFilePickerCtrl(m_repairPanel, wxID_ANY, "", "Save output as...",
                                               "*", wxDefaultPosition, wxDefaultSize,
                                               wxFLP_SAVE | wxFLP_USE_TEXTCTRL | wxFLP_OVERWRITE_PROMPT);
        m_repairOutIsFile = new wxCheckBox(m_repairPanel, wxID_ANY, "Output is a file");
        outSizer->Add(m_repairOutDir, 1, wxEXPAND);
        outSizer->Add(m_repairOutFile, 1, wxEXPAND);
        outSizer->Add(m_repairOutIsFile, 0, wxLEFT, 5);
        gb->Add(outSizer, 1, wxEXPAND);
        m_repairOutFile->Hide();
        m_repairOutIsFile->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            bool isFile = m_repairOutIsFile->GetValue();
            m_repairOutDir->Show(!isFile);
            m_repairOutFile->Show(isFile);
            m_repairDamagedLabel->Show(isFile);
            m_repairDamaged->Show(isFile);
            m_repairPanel->Layout();
        });
        gb->Show(m_repairDamagedLabel, false);
        gb->Show(m_repairDamaged, false);

        gb->Add(new wxStaticText(m_repairPanel, wxID_ANY, "Jobs (0 = auto):"));
        m_repairJobs = new wxSpinCtrl(m_repairPanel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                      wxSP_ARROW_KEYS, 0, 128, 0);
        gb->Add(m_repairJobs, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_repairPanel, wxID_ANY, "Max memory (e.g. 512M, 1G, 0 = auto):"));
        m_repairMaxMem = new wxTextCtrl(m_repairPanel, wxID_ANY, "0");
        gb->Add(m_repairMaxMem, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_repairPanel, wxID_ANY, ""));
        auto* cbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_repairForce = new wxCheckBox(m_repairPanel, wxID_ANY, "Force");
        m_repairDebug = new wxCheckBox(m_repairPanel, wxID_ANY, "Debug");
        m_repairTiming = new wxCheckBox(m_repairPanel, wxID_ANY, "Timing");
        cbSizer->Add(m_repairForce, 0, wxRIGHT, 10);
        cbSizer->Add(m_repairDebug, 0, wxRIGHT, 10);
        cbSizer->Add(m_repairTiming);
        gb->Add(cbSizer);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_repairPanel->SetSizer(s);
        m_notebook->AddPage(m_repairPanel, "Repair");
    }

    void CreateAddTab() {
        m_addPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_addPanel, wxID_ANY, "Archive (.whpar):"));
        m_addArchive = new wxFilePickerCtrl(m_addPanel, wxID_ANY, "", "Select parity archive",
                                            "*.whpar", wxDefaultPosition, wxDefaultSize,
                                            wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        gb->Add(m_addArchive, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_addPanel, wxID_ANY, "Additional overhead (e.g. 0.05):"));
        m_addOverhead = new wxSpinCtrlDouble(m_addPanel, wxID_ANY, "0.05", wxDefaultPosition, wxDefaultSize,
                                             wxSP_ARROW_KEYS, 0.01, 1.0, 0.05, 0.01);
        gb->Add(m_addOverhead, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_addPanel, wxID_ANY, "Jobs (0 = auto):"));
        m_addJobs = new wxSpinCtrl(m_addPanel, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 0, 128, 0);
        gb->Add(m_addJobs, 0, wxEXPAND);

        gb->Add(new wxStaticText(m_addPanel, wxID_ANY, ""));
        auto* cbSizer = new wxBoxSizer(wxHORIZONTAL);
        m_addForce = new wxCheckBox(m_addPanel, wxID_ANY, "Force");
        m_addDebug = new wxCheckBox(m_addPanel, wxID_ANY, "Debug");
        cbSizer->Add(m_addForce, 0, wxRIGHT, 10);
        cbSizer->Add(m_addDebug);
        gb->Add(cbSizer);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_addPanel->SetSizer(s);
        m_notebook->AddPage(m_addPanel, "Add");
    }

    void CreateInfoTab() {
        m_infoPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_infoPanel, wxID_ANY, "Archive (.whpar):"));
        m_infoArchive = new wxFilePickerCtrl(m_infoPanel, wxID_ANY, "", "Select parity archive",
                                             "*.whpar", wxDefaultPosition, wxDefaultSize,
                                             wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        gb->Add(m_infoArchive, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_infoPanel, wxID_ANY, "Source directory (optional):"));
        m_infoSourceDir = new wxDirPickerCtrl(m_infoPanel, wxID_ANY, "", "Select source directory",
                                              wxDefaultPosition, wxDefaultSize,
                                              wxDIRP_USE_TEXTCTRL);
        gb->Add(m_infoSourceDir, 1, wxEXPAND);

        gb->Add(new wxStaticText(m_infoPanel, wxID_ANY, ""));
        m_infoDebug = new wxCheckBox(m_infoPanel, wxID_ANY, "Debug");
        gb->Add(m_infoDebug);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_infoPanel->SetSizer(s);
        m_notebook->AddPage(m_infoPanel, "Info");
    }

    void CreateListTab() {
        m_listPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* gb = new wxFlexGridSizer(2, 10, 10);
        gb->AddGrowableCol(1);

        gb->Add(new wxStaticText(m_listPanel, wxID_ANY, "Archive (.whpar):"));
        m_listArchive = new wxFilePickerCtrl(m_listPanel, wxID_ANY, "", "Select parity archive",
                                             "*.whpar", wxDefaultPosition, wxDefaultSize,
                                             wxFLP_OPEN | wxFLP_USE_TEXTCTRL | wxFLP_FILE_MUST_EXIST);
        gb->Add(m_listArchive, 1, wxEXPAND);

        s->Add(gb, 0, wxEXPAND | wxALL, 10);
        m_listPanel->SetSizer(s);
        m_notebook->AddPage(m_listPanel, "List");
    }

    void CreateSettingsTab() {
        m_settingsPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);

        auto* assocSizer = new wxBoxSizer(wxHORIZONTAL);
        m_settingsAssoc = new wxCheckBox(m_settingsPanel, wxID_ANY,
            "Associate .whpar files with whpar GUI");
        assocSizer->Add(m_settingsAssoc);
        s->Add(assocSizer, 0, wxALL, 10);

        m_settingsStatus = new wxStaticText(m_settingsPanel, wxID_ANY, "",
            wxDefaultPosition, wxDefaultSize,
            wxST_NO_AUTORESIZE);
        s->Add(m_settingsStatus, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        s->AddStretchSpacer();
        m_settingsPanel->SetSizer(s);
        m_notebook->AddPage(m_settingsPanel, "Settings");

        m_settingsAssoc->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
#if defined(_WIN32)
            wxString exePath = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetFullPath();
            if (m_settingsAssoc->GetValue()) {
                HKEY hkRoot = HKEY_CURRENT_USER;
                wxString regRoot = "Software\\Classes";
                HKEY hkClasses;
                if (RegCreateKeyExW(hkRoot, L"Software\\Classes", 0, nullptr,
                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkClasses, nullptr) == ERROR_SUCCESS) {
                    HKEY hkExt;
                    if (RegCreateKeyExW(hkClasses, L".whpar", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkExt, nullptr) == ERROR_SUCCESS) {
                        LPCWSTR val = L"whparfile";
                        RegSetValueExW(hkExt, nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(val), (wcslen(val) + 1) * sizeof(wchar_t));
                        RegCloseKey(hkExt);
                    }
                    HKEY hkProg;
                    if (RegCreateKeyExW(hkClasses, L"whparfile", 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkProg, nullptr) == ERROR_SUCCESS) {
                        LPCWSTR desc = L"whpar Archive";
                        RegSetValueExW(hkProg, nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(desc), (wcslen(desc) + 1) * sizeof(wchar_t));
                        HKEY hkIcon;
                        if (RegCreateKeyExW(hkProg, L"DefaultIcon", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkIcon, nullptr) == ERROR_SUCCESS) {
                            wxString iconPath = exePath + wxString::Format(L",-%d", IDI_WHPARFILE);
                            RegSetValueExW(hkIcon, nullptr, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(iconPath.wc_str()),
                                (iconPath.length() + 1) * sizeof(wchar_t));
                            RegCloseKey(hkIcon);
                        }
                        HKEY hkCmd;
                        if (RegCreateKeyExW(hkProg, L"shell\\open\\command", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkCmd, nullptr) == ERROR_SUCCESS) {
                            wxString cmd = L"\"" + exePath + L"\" \"%1\"";
                            RegSetValueExW(hkCmd, nullptr, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmd.wc_str()),
                                (cmd.length() + 1) * sizeof(wchar_t));
                            RegCloseKey(hkCmd);
                        }
                        RegCloseKey(hkProg);
                    }
                    RegCloseKey(hkClasses);
                }
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                m_settingsStatus->SetLabel(".whpar association registered for current user.");
            } else {
                HKEY hkRoot = HKEY_CURRENT_USER;
                wxString regRoot = "Software\\Classes";
                HKEY hkClasses;
                if (RegOpenKeyExW(hkRoot, L"Software\\Classes", 0, KEY_WRITE, &hkClasses) == ERROR_SUCCESS) {
                    RegDeleteTreeW(hkClasses, L"whparfile");
                    RegDeleteTreeW(hkClasses, L".whpar");
                    RegCloseKey(hkClasses);
                }
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                m_settingsStatus->SetLabel(".whpar association removed.");
            }
            wxMessageBox("File association updated.\n\n"
                "Changes apply to the current user only.\n"
                "You may need to restart Explorer for the icon to update.",
                "Settings", wxOK | wxICON_INFORMATION, m_settingsPanel);
#else
            m_settingsStatus->SetLabel("File association not supported on this platform.");
#endif
        });

        // Check current state
        bool alreadyAssociated = false;
#if defined(_WIN32)
        {
            HKEY hkExt;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.whpar",
                0, KEY_READ, &hkExt) == ERROR_SUCCESS) {
                alreadyAssociated = true;
                RegCloseKey(hkExt);
            }
        }
#endif
        m_settingsAssoc->SetValue(alreadyAssociated);
    }

    void CreateAboutTab() {
        m_aboutPanel = new wxPanel(m_notebook);
        auto* s = new wxBoxSizer(wxVERTICAL);
        auto* txt = new wxStaticText(m_aboutPanel, wxID_ANY,
            wxString::Format("whpar v%s\n\nCopyright (C) 2026 Edward Sloter", WHPAR_VERSION),
            wxDefaultPosition, wxDefaultSize,
            wxALIGN_CENTER_HORIZONTAL);
        wxFont f = txt->GetFont();
        f.SetPointSize(f.GetPointSize() + 4);
        txt->SetFont(f);
        s->AddStretchSpacer();
        s->Add(txt, 0, wxALIGN_CENTER | wxALL, 20);
        s->AddStretchSpacer();
        m_aboutPanel->SetSizer(s);
        m_notebook->AddPage(m_aboutPanel, "About");
    }
};

// ── App ──
class WhparApp : public wxApp {
public:
    bool OnInit() override {
        auto* frame = new WhparFrame();
        frame->Show(true);
        if (argc > 1) {
            wxString arg = argv[1];
            if (arg.Lower().EndsWith(".whpar"))
                frame->LoadArchive(arg);
        }
        return true;
    }
};

wxIMPLEMENT_APP(WhparApp);

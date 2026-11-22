//-----------------------------------------------------------------------------
// The text window's chat screen: a conversation with Claude, which drives
// SolveSpace through the MCP server. The session and transcript are kept in
// SS.chat (the backend is src/mcp/chat.cpp), so they carry on while other
// screens are shown.
//-----------------------------------------------------------------------------
#include "solvespace.h"

namespace SolveSpace {

void TextWindow::ScreenShowChat(int link, uint32_t v) {
    SS.TW.GoToScreen(Screen::CHAT);
    // Ready to type.
    SS.TW.chatInput.focus = true;
    SS.TW.chatInput.scrollToBottom = true;
}

void TextWindow::ScreenChatAction(int link, uint32_t v) {
    switch(link) {
        case 'm':   // the input line
            SS.TW.chatInput.focus = true;
            break;
        case 's':
            if(SS.chat.interrupt) SS.chat.interrupt();
            break;
        case 'n':
            if(SS.chat.reset) SS.chat.reset();
            SS.TW.chatInput.focus = true;
            break;
    }
}

// The aliases offered (Claude Code resolves them to the newest model of
// each family); index 0 is Claude Code's default.
static const char *ChatModelChoices[] = { "", "fable", "opus", "sonnet", "haiku" };

void TextWindow::ScreenChatModel(int link, uint32_t v) {
    switch(link) {
        case 'c':   // [change]
            SS.TW.chatChoosingModel = !SS.TW.chatChoosingModel;
            break;
        case 'k':   // one of the choices
            if(v < sizeof(ChatModelChoices) / sizeof(ChatModelChoices[0]) &&
               SS.chat.setModel) {
                SS.chat.setModel(ChatModelChoices[v]);
            }
            SS.TW.chatChoosingModel = false;
            break;
        case 'o':   // other...: type a name
            SS.TW.edit.meaning = Edit::CHAT_MODEL;
            SS.TW.ShowEditControl(3, SS.chat.model);
            break;
    }
}

// Claude Code's effort levels: how much Claude thinks before it acts. Lower is
// faster, higher helps with hard design problems.
static const char *ChatEffortChoices[] = { "low", "medium", "high", "xhigh", "max" };

void TextWindow::ScreenChatEffort(int link, uint32_t v) {
    switch(link) {
        case 'c':   // [change]
            SS.TW.chatChoosingEffort = !SS.TW.chatChoosingEffort;
            break;
        case 'k':   // one of the choices
            if(v < sizeof(ChatEffortChoices) / sizeof(ChatEffortChoices[0]) &&
               SS.chat.setEffort) {
                SS.chat.setEffort(ChatEffortChoices[v]);
            }
            SS.TW.chatChoosingEffort = false;
            break;
    }
}

// Splits UTF-8 text into lines of at most `width` characters, at spaces
// where possible.
static void WrapText(const std::string &text, int width, std::vector<std::string> *lines) {
    if(width < 10) width = 10;
    size_t start = 0;
    while(start <= text.size()) {
        size_t end = text.find('\n', start);
        if(end == std::string::npos) end = text.size();
        std::string para;
        for(size_t i = start; i < end; i++) {
            char c = text[i];
            if(c == '\t') {
                para += "    ";
            } else if((unsigned char)c >= 0x20 || (c & 0x80)) {
                para += c;
            }
        }
        start = end + 1;

        // Wrap the paragraph, counting characters (not bytes).
        std::string line;
        int lineChars = 0;
        size_t lastSpace = std::string::npos;   // byte offset in `line`
        int charsAtSpace = 0;
        for(size_t i = 0; i < para.size();) {
            size_t len = 1;
            unsigned char lead = (unsigned char)para[i];
            if(lead >= 0xF0) len = 4;
            else if(lead >= 0xE0) len = 3;
            else if(lead >= 0xC0) len = 2;
            std::string ch = para.substr(i, len);
            i += len;

            if(lineChars == width) {
                if(ch == " ") {
                    lines->push_back(line);
                    line.clear();
                    lineChars = 0;
                    lastSpace = std::string::npos;
                    continue;
                } else if(lastSpace != std::string::npos && lastSpace > 0) {
                    lines->push_back(line.substr(0, lastSpace));
                    line = line.substr(lastSpace + 1);
                    lineChars -= charsAtSpace + 1;
                } else {
                    lines->push_back(line);
                    line.clear();
                    lineChars = 0;
                }
                lastSpace = std::string::npos;
            }
            if(ch == " ") {
                lastSpace = line.size();
                charsAtSpace = lineChars;
            }
            line += ch;
            lineChars++;
        }
        lines->push_back(line);
        if(end == text.size()) break;
    }
}

void TextWindow::ShowChat() {
    typedef SolveSpaceUI::ChatEntry::Kind Kind;

    if(!SS.chat.send) {
        Printf(true, "%FtChat with Claude%E");
        Printf(false, "  This build of SolveSpace doesn't have the MCP server, "
                      "which the chat needs.");
        return;
    }

    // Fit the text to the window's width.
    int cols = MAX_COLS - 1;
    if(window) {
        double width, height;
        window->GetContentSize(&width, &height);
        // Leave room for the scrollbar.
        cols = (int)((width - LEFT_MARGIN - 28) / CHAR_WIDTH_);
        cols = max(30, min(cols, (int)MAX_COLS - 1));
    }
    const int indent = 3;
    int wrapWidth = cols - indent;

    if(SS.chat.entries.empty()) {
        Printf(true, "%FtChat with Claude%E");
    } else {
        Printf(true, "%FtChat with Claude%E  %Fl%Ln%f[new chat]%E", &ScreenChatAction);
    }
    if(!SS.chat.status.empty()) {
        std::vector<std::string> status;
        WrapText(SS.chat.status, cols - 1, &status);
        for(const std::string &l : status) Printf(false, " %Fg%s%E", l.c_str());
    }

    // The model: the one in use, else the one asked for.
    std::string model = SS.chat.running ? SS.chat.activeModel : "";
    if(model.empty()) model = SS.chat.model.empty() ? "default" : SS.chat.model;
    Printf(false, " %Fgmodel%E %Fi%s%E  %Fl%Lc%f[%s]%E", model.c_str(),
           &ScreenChatModel, chatChoosingModel ? "close" : "change");
    if(chatChoosingModel) {
        Printf(false, "%Fg   pick one; it's used from the next message%E");
        for(uint32_t i = 0; i < sizeof(ChatModelChoices) / sizeof(ChatModelChoices[0]); i++) {
            const char *name = ChatModelChoices[i][0] ? ChatModelChoices[i] : "default";
            bool chosen = (SS.chat.model == ChatModelChoices[i]);
            if(chosen) {
                Printf(false, "   %Fs" RADIO_TRUE " %s%E", name);
            } else {
                Printf(false, "   %Fl%Lk%D%f" RADIO_FALSE " %s%E", i, &ScreenChatModel, name);
            }
        }
        bool other = true;
        for(const char *c : ChatModelChoices) if(SS.chat.model == c) other = false;
        if(other) {
            Printf(false, "   %Fs" RADIO_TRUE " %s%E  %Fl%Lo%f[edit]%E", SS.chat.model.c_str(),
                   &ScreenChatModel);
        } else {
            Printf(false, "   %Fl%Lo%f" RADIO_FALSE " other...%E", &ScreenChatModel);
        }
    }

    std::string effort = SS.chat.effort.empty() ? "default" : SS.chat.effort;
    Printf(false, " %Fgeffort%E %Fi%s%E  %Fl%Lc%f[%s]%E", effort.c_str(),
           &ScreenChatEffort, chatChoosingEffort ? "close" : "change");
    if(chatChoosingEffort) {
        Printf(false, "%Fg   lower is faster; it's used from the next message%E");
        for(uint32_t i = 0; i < sizeof(ChatEffortChoices) / sizeof(ChatEffortChoices[0]); i++) {
            if(SS.chat.effort == ChatEffortChoices[i]) {
                Printf(false, "   %Fs" RADIO_TRUE " %s%E", ChatEffortChoices[i]);
            } else {
                Printf(false, "   %Fl%Lk%D%f" RADIO_FALSE " %s%E", i, &ScreenChatEffort,
                       ChatEffortChoices[i]);
            }
        }
    }

    struct Line {
        char        color;
        const char *gutter;
        std::string text;
    };
    std::vector<Line> lines;
    std::vector<std::string> wrapped;

    Kind prev = Kind::NOTE;
    bool first = true;
    for(const SolveSpaceUI::ChatEntry &entry : SS.chat.entries) {
        // A blank line before each of the user's messages and Claude's
        // replies, but not between consecutive tool calls.
        bool spaced = !(entry.kind == Kind::TOOL && prev == Kind::TOOL) &&
                      !(entry.kind == Kind::NOTE && prev != Kind::USER);
        if(first || spaced) lines.push_back({ 'd', "", "" });
        first = false;
        prev = entry.kind;

        char color = 'd';
        const char *gutter = "";
        switch(entry.kind) {
            case Kind::USER:    color = 'i'; gutter = ">"; break;
            case Kind::CLAUDE:  color = 'd'; break;
            case Kind::TOOL:    color = 'g'; gutter = "*"; break;
            case Kind::NOTE:    color = 'g'; break;
            case Kind::FAILURE: color = 'x'; gutter = "!"; break;
        }
        wrapped.clear();
        WrapText(entry.text, wrapWidth, &wrapped);
        for(size_t i = 0; i < wrapped.size(); i++) {
            lines.push_back({ color, i == 0 ? gutter : "", wrapped[i] });
        }
    }

    // Keep within the window's row limit: show the newest lines.
    const size_t maxLines = MAX_ROWS - 40;
    size_t firstLine = 0;
    if(lines.size() > maxLines) {
        firstLine = lines.size() - maxLines;
        Printf(false, "%Fg  (earlier messages not shown)%E");
    }
    for(size_t i = firstLine; i < lines.size(); i++) {
        const Line &l = lines[i];
        if(l.text.empty() && l.gutter[0] == '\0') {
            Printf(false, "");
        } else {
            // (Printf has no field widths.)
            std::string gutter = l.gutter;
            gutter.resize(indent - 1, ' ');
            Printf(false, " %Fp%s%s%E", l.color, gutter.c_str(), l.text.c_str());
        }
    }

    // (The input is in the footer; see ShowChatFooter().)
}

// The chat's footer, pinned to the bottom of the window: a line of status,
// which doesn't change height (so the input doesn't jump), and the input.
void TextWindow::ShowChatFooter() {
    footerRow = rows;
    if(SS.chat.pending > 0) {
        std::string queued = (SS.chat.pending > 1)
                             ? ssprintf(" (%d more queued)", SS.chat.pending - 1) : "";
        // E.g. "thinking, 2:41, ~16.2k tokens".
        std::string activity = SS.chat.activity.empty() ? "working..." : SS.chat.activity;
        Printf(false, "%Fg   %s%s%E  %Fl%Ls%f[stop]%E", activity.c_str(), queued.c_str(),
               &ScreenChatAction);
    } else {
        Printf(false, "%Fg   Enter sends, Shift+Enter starts a new line%E");
    }
    // Clicking the prompt gives the input the keyboard (or opens it, where
    // the platform has no editor that can stay open).
    Printf(false, " %Fl%Lm%f>%E", &ScreenChatAction);
    chatInput.row = rows - 1;
}

void TextWindow::PlaceChatInput(bool focus) {
    if(!window || footerRow < 0) return;

    double width, height;
    window->GetContentSize(&width, &height);
    double x = LEFT_MARGIN + 3*CHAR_WIDTH_;
    double y = RowY(chatInput.row) + 2;
    double w = max(100.0, width - x - 20);
    // Up to about a third of the window, then the editor scrolls.
    double maxHeight = max((double)LINE_HEIGHT, floor(height / 3));

    bool moved = (x != chatInput.x || y != chatInput.y || w != chatInput.width ||
                  maxHeight != chatInput.maxHeight);
    chatInput.x         = x;
    chatInput.y         = y;
    chatInput.width     = w;
    chatInput.maxHeight = maxHeight;

    if(chatInput.open && window->IsEditorVisible()) {
        if(focus) {
            window->ShowMultilineEditor(x, y, w, maxHeight, LINE_HEIGHT - 4,
                                        SS.chat.draft, /*focus=*/true);
        } else if(moved) {
            window->MoveEditor(x, y, w, maxHeight);
        }
    } else {
        // (Open before showing it, so that the size it reports is taken.)
        chatInput.open = true;
        window->ShowMultilineEditor(x, y, w, maxHeight, LINE_HEIGHT - 4,
                                    SS.chat.draft, focus);
        // Platforms without a multi-line editor only open one to type in.
        chatInput.open = window->IsEditorVisible();
    }
}

void TextWindow::HideChatInput() {
    if(!chatInput.open) return;
    chatInput.open = false;
    if(window) {
        window->HideEditor();
        window->Invalidate();
    }
}

void TextWindow::ChatInputChanged(const std::string &text, double height) {
    if(!chatInput.open) return;
    SS.chat.draft = text;
    if(height == chatInput.height) return;

    // The footer grows (or shrinks) upwards with the editor.
    bool atBottom = (scrollPos >= ScrollBottom() - ScrollPage() - 1);
    chatInput.height = height;
    if(atBottom) scrollPos = ScrollBottom();
    if(window) {
        Resize();
        PlaceChatInput(/*focus=*/false);
    }
}

void TextWindow::ChatInputDone(std::string text) {
    // Without trailing blank lines; and nothing to send if it's all blank.
    size_t end = text.find_last_not_of(" \t\r\n");
    if(end == std::string::npos) return;
    text.erase(end + 1);

    // Empty the input, keeping it open to write the next message.
    SS.chat.draft.clear();
    if(window) {
        window->ShowMultilineEditor(chatInput.x, chatInput.y, chatInput.width,
                                    chatInput.maxHeight, LINE_HEIGHT - 4, "",
                                    /*focus=*/true);
        chatInput.open = window->IsEditorVisible();
    }
    chatInput.scrollToBottom = true;
    if(SS.chat.send) SS.chat.send(text);
    SS.ScheduleShowTW();
}

} // namespace SolveSpace

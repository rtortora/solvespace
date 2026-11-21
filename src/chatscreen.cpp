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
    // Ready to type, unless Claude is still replying.
    SS.TW.chatOpenEditor = !SS.chat.busy;
}

void TextWindow::ScreenChatAction(int link, uint32_t v) {
    switch(link) {
        case 'm':   // the input line
            SS.TW.chatOpenEditor = true;
            break;
        case 's':
            if(SS.chat.interrupt) SS.chat.interrupt();
            break;
        case 'n':
            if(SS.chat.reset) SS.chat.reset();
            SS.TW.chatOpenEditor = true;
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

    struct Line {
        char        color;
        const char *gutter;
        std::string text;
    };
    std::vector<Line> lines;
    std::vector<std::string> wrapped;

    if(SS.chat.entries.empty()) {
        WrapText("Ask Claude to build, change or explain the model. It works "
                 "through SolveSpace's MCP server and can only use SolveSpace's "
                 "tools; you see every change in the window, and Ctrl+Z undoes it. "
                 "Needs Claude Code (the `claude` command), signed in.",
                 wrapWidth, &wrapped);
        lines.push_back({ 'd', "", "" });
        for(std::string &l : wrapped) lines.push_back({ 'g', "", l });
    }

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

    Printf(false, "");
    if(SS.chat.busy) {
        Printf(false, "%Fg   working...%E  %Fl%Ls%f[stop]%E", &ScreenChatAction);
    } else {
        Printf(false, "%Bd %Fl>%E  %Fl%Lm%f[click here to type a message]%E",
               &ScreenChatAction);
        chatInputHalfRow = top[rows - 1];
    }
}

} // namespace SolveSpace

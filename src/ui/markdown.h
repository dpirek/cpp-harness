#pragma once
#include <string>
#include <vector>

namespace chat {
enum MdStyle : unsigned { MdBold=1,MdItalic=2,MdCode=4,MdStrike=8,MdLink=16,MdImage=32 };
struct MdRun {std::wstring text;unsigned style=0;std::wstring href;};
struct MdCell {std::vector<MdRun> runs;bool header=false;int align=0;};
enum class MdKind {Paragraph,Heading,Code,Rule,Table};
struct MdBlock {
    MdKind kind=MdKind::Paragraph;
    std::vector<MdRun> runs;
    std::wstring text,language,marker;
    int heading=0,indent=0,quote=0;
    std::vector<std::vector<MdCell>> rows;
};
std::vector<MdBlock> ParseMarkdown(const std::wstring& text);
}

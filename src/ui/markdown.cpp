#define NOMINMAX
#define MD4C_USE_UTF16
#include "ui/markdown.h"
#include <md4c/md4c.h>
extern "C" {
#include <md4c/entity.h>
}
#include <optional>
#include <cwctype>
#include <algorithm>

namespace chat {
namespace {
void Codepoint(std::wstring& text,unsigned value) {
    if(!value||value>0x10ffff||(value>=0xd800&&value<=0xdfff))value=0xfffd;
    if(value<=0xffff)text+=static_cast<wchar_t>(value);
    else {value-=0x10000;text+=static_cast<wchar_t>(0xd800+(value>>10));text+=static_cast<wchar_t>(0xdc00+(value&1023));}
}
std::wstring Entity(const std::wstring& source) {
    if(source.size()>3&&source[1]==L'#') {
        auto end=source.size()-1;size_t start=2;unsigned base=10,value=0;
        if(source[start]==L'x'||source[start]==L'X'){base=16;++start;}
        for(size_t i=start;i<end;++i){auto c=source[i];unsigned digit=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:c>=L'A'&&c<=L'F'?c-L'A'+10:99;if(digit>=base)return source;value=value>0x10ffff?0x110000:value*base+digit;}
        std::wstring result;Codepoint(result,value);return result;
    }
    std::string ascii;for(auto c:source){if(c>127)return source;ascii+=static_cast<char>(c);}auto entity=entity_lookup(ascii.data(),ascii.size());
    if(!entity)return source;std::wstring result;Codepoint(result,entity->codepoints[0]);if(entity->codepoints[1])Codepoint(result,entity->codepoints[1]);return result;
}
std::wstring Attribute(const MD_ATTRIBUTE& attribute) {
    std::wstring result;
    for(unsigned i=0;attribute.size&&attribute.substr_offsets[i]<attribute.size;++i) {
        auto begin=attribute.substr_offsets[i],end=attribute.substr_offsets[i+1];
        auto text=std::wstring(attribute.text+begin,end-begin);
        result+=attribute.substr_types[i]==MD_TEXT_ENTITY?Entity(text):attribute.substr_types[i]==MD_TEXT_NULLCHAR?L"\ufffd":text;
    }
    return result;
}
struct Parser {
    struct List {bool ordered;unsigned next;};
    struct Span {unsigned style;std::wstring href;};
    std::vector<MdBlock> blocks;
    std::vector<List> lists;
    std::vector<Span> spans;
    std::optional<size_t> active,table;
    unsigned style=0;std::wstring href;int quote=0;bool cell=false;
    int imageDepth=0;std::wstring imageSource,imageAlt;
    void New(MdKind kind) {
        MdBlock block;block.kind=kind;block.indent=static_cast<int>(lists.size());block.quote=quote;
        blocks.push_back(std::move(block));active=blocks.size()-1;
    }
    void Append(std::wstring text) {
        if(text.empty())return;
        if(imageDepth){imageAlt+=text;return;}
        if(table&&cell){auto& runs=blocks[*table].rows.back().back().runs;Add(runs,std::move(text));return;}
        if(!active)New(MdKind::Paragraph);
        auto& block=blocks[*active];if(block.kind==MdKind::Code){block.text+=text;return;}
        Add(block.runs,std::move(text));
    }
    void Add(std::vector<MdRun>& runs,std::wstring text) {
        if(!runs.empty()&&runs.back().style==style&&runs.back().href==href)runs.back().text+=text;
        else runs.push_back({std::move(text),style,href});
    }
    static int EnterBlock(MD_BLOCKTYPE type,void* detail,void* context) {
        auto& p=*static_cast<Parser*>(context);
        switch(type) {
        case MD_BLOCK_QUOTE:p.active.reset();++p.quote;break;
        case MD_BLOCK_UL:p.active.reset();p.lists.push_back({false,0});break;
        case MD_BLOCK_OL:p.active.reset();p.lists.push_back({true,static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start});break;
        case MD_BLOCK_LI: {
            p.New(MdKind::Paragraph);auto& block=p.blocks.back();auto& list=p.lists.back();
            auto item=static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            block.marker=item->is_task?(item->task_mark==L' '?L"\u2610":L"\u2611"):list.ordered?std::to_wstring(list.next++)+L".":L"\u2022";break;
        }
        case MD_BLOCK_P:
            if(!p.active||p.blocks[*p.active].kind!=MdKind::Paragraph||!p.blocks[*p.active].runs.empty())p.New(MdKind::Paragraph);
            break;
        case MD_BLOCK_H:p.New(MdKind::Heading);p.blocks.back().heading=static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);break;
        case MD_BLOCK_HR:p.New(MdKind::Rule);p.active.reset();break;
        case MD_BLOCK_CODE: {
            p.New(MdKind::Code);p.blocks.back().language=Attribute(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang);
            auto& language=p.blocks.back().language;std::transform(language.begin(),language.end(),language.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});break;
        }
        case MD_BLOCK_TABLE:p.New(MdKind::Table);p.table=p.active;p.active.reset();break;
        case MD_BLOCK_TR:if(p.table)p.blocks[*p.table].rows.emplace_back();break;
        case MD_BLOCK_TH:case MD_BLOCK_TD:
            if(p.table){p.blocks[*p.table].rows.back().push_back({{},type==MD_BLOCK_TH,static_cast<int>(static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align)});p.cell=true;}break;
        default:break;
        }
        return 0;
    }
    static int LeaveBlock(MD_BLOCKTYPE type,void*,void* context) {
        auto& p=*static_cast<Parser*>(context);
        switch(type) {
        case MD_BLOCK_QUOTE:p.active.reset();--p.quote;break;
        case MD_BLOCK_UL:case MD_BLOCK_OL:p.active.reset();p.lists.pop_back();break;
        case MD_BLOCK_P:case MD_BLOCK_H:case MD_BLOCK_CODE:case MD_BLOCK_LI:p.active.reset();break;
        case MD_BLOCK_TD:case MD_BLOCK_TH:p.cell=false;break;
        case MD_BLOCK_TABLE:p.table.reset();p.active.reset();break;
        default:break;
        }
        return 0;
    }
    static int EnterSpan(MD_SPANTYPE type,void* detail,void* context) {
        auto& p=*static_cast<Parser*>(context);p.spans.push_back({p.style,p.href});
        switch(type) {
        case MD_SPAN_STRONG:p.style|=MdBold;break;
        case MD_SPAN_EM:p.style|=MdItalic;break;
        case MD_SPAN_CODE:p.style|=MdCode;break;
        case MD_SPAN_DEL:p.style|=MdStrike;break;
        case MD_SPAN_A:p.style|=MdLink;p.href=Attribute(static_cast<MD_SPAN_A_DETAIL*>(detail)->href);break;
        case MD_SPAN_IMG:
            if(p.imageDepth++==0){p.imageSource=Attribute(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);p.imageAlt.clear();}break;
        default:break;
        }
        return 0;
    }
    static int LeaveSpan(MD_SPANTYPE type,void*,void* context) {
        auto& p=*static_cast<Parser*>(context);
        if(type==MD_SPAN_IMG&&--p.imageDepth==0) {
            if(!p.active&&!(p.table&&p.cell))p.New(MdKind::Paragraph);
            auto& runs=p.table&&p.cell?p.blocks[*p.table].rows.back().back().runs:p.blocks[*p.active].runs;
            runs.push_back({p.imageAlt,MdImage,p.imageSource});
        }
        p.style=p.spans.back().style;p.href=p.spans.back().href;p.spans.pop_back();return 0;
    }
    static int Text(MD_TEXTTYPE type,const MD_CHAR* text,MD_SIZE size,void* context) {
        auto& p=*static_cast<Parser*>(context);
        if(type==MD_TEXT_BR)p.Append(L"\n");
        else if(type==MD_TEXT_SOFTBR)p.Append(L" ");
        else if(type==MD_TEXT_NULLCHAR)p.Append(L"\ufffd");
        else if(type==MD_TEXT_ENTITY)p.Append(Entity(std::wstring(text,size)));
        else p.Append(std::wstring(text,size));
        return 0;
    }
};
}
std::vector<MdBlock> ParseMarkdown(const std::wstring& text) {
    Parser context;MD_PARSER parser{};parser.flags=MD_DIALECT_GITHUB|MD_FLAG_NOHTML;
    parser.enter_block=Parser::EnterBlock;parser.leave_block=Parser::LeaveBlock;parser.enter_span=Parser::EnterSpan;parser.leave_span=Parser::LeaveSpan;parser.text=Parser::Text;
    if(md_parse(text.data(),static_cast<MD_SIZE>(text.size()),&parser,&context)!=0) {
        MdBlock fallback;fallback.runs.push_back({text,0,{}});return {std::move(fallback)};
    }
    return std::move(context.blocks);
}
}

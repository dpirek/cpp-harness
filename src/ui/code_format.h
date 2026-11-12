#pragma once
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace chat {
struct CodeBlock { std::wstring text, language; bool code=false; };
inline std::vector<CodeBlock> FormatBlocks(const std::wstring& text) {
    std::vector<CodeBlock> blocks;
    CodeBlock current;
    wchar_t fenceChar=0;size_t fenceSize=0;
    size_t position=0;
    while(position<text.size()) {
        auto end=text.find(L'\n',position);bool newline=end!=text.npos;
        if(!newline)end=text.size();
        auto line=text.substr(position,end-position);
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        auto first=line.find_first_not_of(L' ');
        size_t count=0;
        if(first!=line.npos&&first<=3&&(line[first]==L'`'||line[first]==L'~'))while(first+count<line.size()&&line[first+count]==line[first])++count;
        bool fence=count>=3;
        if(current.code)fence=fence&&line[first]==fenceChar&&count>=fenceSize&&line.find_first_not_of(L" \t",first+count)==line.npos;
        else if(fence&&line[first]==L'`'&&line.find(L'`',first+count)!=line.npos)fence=false;
        if(fence) {
            if(!current.text.empty()||current.code)blocks.push_back(current);
            if(current.code){current={};fenceChar=0;fenceSize=0;}
            else {
                fenceChar=line[first];fenceSize=count;
                current={L"",line.substr(first+count),true};
                auto start=current.language.find_first_not_of(L" \t");
                current.language=start==std::wstring::npos?L"":current.language.substr(start);
                auto stop=current.language.find_first_of(L" \t\r");
                if(stop!=std::wstring::npos)current.language.resize(stop);
                std::transform(current.language.begin(),current.language.end(),current.language.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
            }
        } else {current.text+=line;if(newline)current.text+=L'\n';}
        position=newline?end+1:end;
    }
    if(!current.text.empty()||current.code)blocks.push_back(current);
    return blocks;
}
enum class CodeColor { Plain, String, Comment, Keyword, Variable, Tag, Number };
inline std::vector<CodeColor> Highlight(const std::wstring& text,const std::wstring& language) {
    std::vector<CodeColor> colors(text.size(),CodeColor::Plain);
    bool xml=language==L"xml"||language==L"html";
    bool ps=language==L"powershell"||language==L"ps"||language==L"ps1"||language==L"pwsh";
    bool json=language==L"json"||language==L"jsonc";
    bool tag=false;
    auto mark=[&](size_t begin,size_t end,CodeColor color){std::fill(colors.begin()+begin,colors.begin()+end,color);};
    for(size_t i=0;i<text.size();) {
        size_t start=i;
        if(json&&text[i]==L'"') {
            ++i;while(i<text.size()){if(text[i]==L'\\'){i=(std::min)(text.size(),i+2);continue;}if(text[i++]==L'"')break;}
            auto next=text.find_first_not_of(L" \t\r\n",i);
            mark(start,i,next!=text.npos&&text[next]==L':'?CodeColor::Variable:CodeColor::String);
        } else if(json&&(iswdigit(text[i])||text[i]==L'-')) {
            ++i;while(i<text.size()&&(iswdigit(text[i])||text[i]==L'.'||text[i]==L'e'||text[i]==L'E'||text[i]==L'+'||text[i]==L'-'))++i;
            mark(start,i,CodeColor::Number);
        } else if(json&&(text.compare(i,4,L"true")==0||text.compare(i,5,L"false")==0||text.compare(i,4,L"null")==0)) {
            i+=text[i]==L'f'?5:4;mark(start,i,CodeColor::Keyword);
        } else if((xml&&text.compare(i,4,L"<!--")==0)||(ps&&text.compare(i,2,L"<#")==0)) {
            auto close=xml?L"-->":L"#>";auto end=text.find(close,i+2);
            i=end==text.npos?text.size():end+std::wstring(close).size();mark(start,i,CodeColor::Comment);
        } else if(ps&&text[i]==L'#') {
            i=text.find(L'\n',i);if(i==text.npos)i=text.size();mark(start,i,CodeColor::Comment);
        } else if((ps||(xml&&tag))&&(text[i]==L'\''||text[i]==L'"')) {
            auto quote=text[i++];while(i<text.size()){if(ps&&text[i]==L'`'){i=(std::min)(text.size(),i+2);continue;}if(text[i++]==quote){if(ps&&quote==L'\''&&i<text.size()&&text[i]==quote){++i;continue;}break;}}
            mark(start,i,CodeColor::String);
        } else if(xml&&text[i]==L'<') {tag=true;colors[i++]=CodeColor::Tag;}
        else if(xml&&text[i]==L'>') {tag=false;colors[i++]=CodeColor::Tag;}
        else if(xml&&tag) {colors[i++]=CodeColor::Tag;}
        else if(ps&&text[i]==L'$') {
            ++i;if(i<text.size()&&text[i]==L'{'){auto end=text.find(L'}',i);i=end==text.npos?text.size():end+1;}
            else while(i<text.size()&&(iswalnum(text[i])||text[i]==L'_'||text[i]==L':'))++i;
            mark(start,i,CodeColor::Variable);
        } else if(ps&&(iswalnum(text[i])||text[i]==L'-')) {
            while(i<text.size()&&(iswalnum(text[i])||text[i]==L'-'||text[i]==L'_'))++i;
            auto word=text.substr(start,i-start),lower=word;
            std::transform(lower.begin(),lower.end(),lower.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
            if(iswdigit(word[0]))mark(start,i,CodeColor::Number);
            else if(word.find(L'-')!=word.npos||std::wstring(L" if else elseif foreach for while function return param try catch throw switch in begin process end ").find(L" "+lower+L" ")!=std::wstring::npos)mark(start,i,CodeColor::Keyword);
        } else ++i;
    }
    return colors;
}
}


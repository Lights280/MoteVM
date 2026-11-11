#include "asm/lexer.hpp"

#include <cstdint>

#include "common/text.hpp"

namespace
{
    int hexDigit(char character)
    {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    }

    void appendUtf8(std::vector<byte> &out, std::uint32_t codePoint)
    {
        if (codePoint < 0x80u)
        {
            out.push_back(static_cast<byte>(codePoint));
        }
        else if (codePoint < 0x800u)
        {
            out.push_back(static_cast<byte>(0xC0u | (codePoint >> 6)));
            out.push_back(static_cast<byte>(0x80u | (codePoint & 0x3Fu)));
        }
        else if (codePoint < 0x10000u)
        {
            out.push_back(static_cast<byte>(0xE0u | (codePoint >> 12)));
            out.push_back(static_cast<byte>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<byte>(0x80u | (codePoint & 0x3Fu)));
        }
        else
        {
            out.push_back(static_cast<byte>(0xF0u | (codePoint >> 18)));
            out.push_back(static_cast<byte>(0x80u | ((codePoint >> 12) & 0x3Fu)));
            out.push_back(static_cast<byte>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<byte>(0x80u | (codePoint & 0x3Fu)));
        }
    }
}

bool ASM::tokenizeLine(std::string_view line, int lineNumber, std::vector<Token> &tokens,
                       TOOL::Diagnostics &diagnostics)
{
    const auto positionAt = [line, lineNumber](std::size_t offset) {
        return TOOL::SourcePosition{lineNumber, static_cast<int>(TOOL::codePointColumn(line, offset))};
    };

    std::size_t index = 0;
    while (index < line.size())
    {
        const char character = line[index];
        if (character == ' ' || character == '\t' || character == '\r' || character == ',')
        {
            ++index;
            continue;
        }
        if (character == ';')
            break;

        if (character == '"')
        {
            Token token;
            token.isString = true;
            token.position = positionAt(index);
            const std::size_t start = index;
            ++index;
            bool closed = false;
            while (index < line.size())
            {
                const char current = line[index];
                if (current == '"')
                {
                    ++index;
                    closed = true;
                    break;
                }
                if (current != '\\')
                {
                    token.bytes.push_back(static_cast<byte>(static_cast<unsigned char>(current)));
                    ++index;
                    continue;
                }

                const std::size_t escapeAt = index;
                ++index;
                if (index >= line.size())
                    break;
                const char escape = line[index++];
                switch (escape)
                {
                case '\\':
                    token.bytes.push_back(static_cast<byte>('\\'));
                    break;
                case '"':
                    token.bytes.push_back(static_cast<byte>('"'));
                    break;
                case 'n':
                    token.bytes.push_back(static_cast<byte>('\n'));
                    break;
                case 'r':
                    token.bytes.push_back(static_cast<byte>('\r'));
                    break;
                case 't':
                    token.bytes.push_back(static_cast<byte>('\t'));
                    break;
                case '0':
                    token.bytes.push_back(static_cast<byte>(0));
                    break;
                case 'x':
                {
                    if (index + 1 >= line.size())
                    {
                        diagnostics.error("E009", positionAt(escapeAt),
                                          "invalid \\x escape: two hex digits required");
                        return false;
                    }
                    const int high = hexDigit(line[index]);
                    const int low = hexDigit(line[index + 1]);
                    if (high < 0 || low < 0)
                    {
                        diagnostics.error("E009", positionAt(escapeAt),
                                          "invalid \\x escape: two hex digits required");
                        return false;
                    }
                    token.bytes.push_back(static_cast<byte>((high << 4) | low));
                    index += 2;
                    break;
                }
                case 'u':
                {
                    if (index >= line.size() || line[index] != '{')
                    {
                        diagnostics.error("E009", positionAt(escapeAt),
                                          "invalid \\u escape: expected '{'");
                        return false;
                    }
                    ++index;
                    std::uint32_t codePoint = 0;
                    int digits = 0;
                    while (index < line.size() && line[index] != '}')
                    {
                        const int digit = hexDigit(line[index]);
                        if (digit < 0 || digits >= 6)
                        {
                            diagnostics.error("E009", positionAt(escapeAt),
                                              "invalid \\u{} code point");
                            return false;
                        }
                        codePoint = (codePoint << 4) | static_cast<std::uint32_t>(digit);
                        ++digits;
                        ++index;
                    }
                    if (index >= line.size() || line[index] != '}' || digits == 0 ||
                        codePoint > 0x10FFFFu ||
                        (codePoint >= 0xD800u && codePoint <= 0xDFFFu))
                    {
                        diagnostics.error("E009", positionAt(escapeAt),
                                          "invalid \\u{} code point");
                        return false;
                    }
                    ++index;
                    appendUtf8(token.bytes, codePoint);
                    break;
                }
                default:
                    diagnostics.error("E009", positionAt(escapeAt),
                                      std::string("invalid escape '\\") + escape + "'");
                    return false;
                }
            }

            if (!closed)
            {
                diagnostics.error("E008", token.position, "unterminated string literal");
                return false;
            }
            token.text = std::string(line.substr(start, index - start));
            tokens.push_back(std::move(token));
            continue;
        }

        const std::size_t start = index;
        while (index < line.size())
        {
            const char current = line[index];
            if (current == ' ' || current == '\t' || current == '\r' || current == ',' ||
                current == ';')
                break;
            ++index;
        }
        if (index != start)
        {
            Token token;
            token.text = std::string(line.substr(start, index - start));
            token.position = positionAt(start);
            tokens.push_back(std::move(token));
        }
    }
    return true;
}


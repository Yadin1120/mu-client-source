//************************************************************************
//
// Decompiled by @myheart, @synth3r
// <https://forum.ragezone.com/members/2000236254.html>
//
//
// FILE: ShopList.cpp
//
//

#include "stdafx.h"
#ifdef KJH_ADD_INGAMESHOP_UI_SYSTEM
#include "ShopList.h"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include "Core/Platform/PathResolve.h"
#endif

namespace
{
// The catalog paths the shop composes are Windows-spelled ("...\data\
// InGameShopScript\<ver>\IBSCategory.txt"). Every other reader in the engine
// funnels through MuResolvePath - _wfopen, CreateFileW, the bitmap loader -
// but these std::ifstream opens went straight to the filesystem, where a
// backslash is an ordinary filename character. The download succeeded and the
// parse then failed on a file "not found" that was sitting right there, which
// is what made the shop look like a network fault on macOS.
//
// The existence check upstream (CListManager::IsScriptFileExist) uses the
// GetFileAttributes shim, which DOES resolve - so the two disagreed, and the
// retry loop re-downloaded all three files on every attempt, forever.
std::filesystem::path ResolveCatalogPath(const wchar_t* szFilePath)
{
#ifdef _WIN32
    return std::filesystem::path(szFilePath);
#else
    return std::filesystem::path(
        MuResolvePath(std::filesystem::path(szFilePath).string().c_str()));
#endif
}
} // namespace

CShopList::CShopList() // OK
{
    this->m_CategoryListPtr = new CShopCategoryList;
    this->m_PackageListPtr = new CShopPackageList;
    this->m_ProductListPtr = new CShopProductList;
}

CShopList::~CShopList() // OK
{
    SAFE_DELETE(m_CategoryListPtr);
    SAFE_DELETE(m_PackageListPtr);
    SAFE_DELETE(m_ProductListPtr);
}

WZResult CShopList::LoadCategroy(const wchar_t* szFilePath) // OK
{
    WZResult result;

    FILE_ENCODE enc = this->IsFileEncodingUtf8(szFilePath);

    std::ifstream ifs;

    ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);

    DWORD LastError = GetLastError();

    for (int n = 0; !ifs.is_open() && n < 10; ++n)
    {
        Sleep(0x64);
        ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);
        LastError = GetLastError();
    }

    char buff[1024] = { 0 };

    if (ifs.is_open())
    {
        this->GetCategoryListPtr()->Clear();

        int linesRead = 0;
        int rowsParsed = 0;

        while (true)
        {
            memset(buff, 0, sizeof(buff));

            if (!ifs.getline(buff, sizeof(buff)))
                break;

            ++linesRead;

            CShopCategory cat;

            const std::wstring data = this->GetDecodedString(buff, enc);

            if (cat.SetCategory(data))
            {
                this->GetCategoryListPtr()->Append(cat);
                ++rowsParsed;
            }
        }

        ifs.close();

        // Make a zero-row decode impossible to mistake for success.
        // If the decoder ever mangles the catalog again, rowsParsed=0 (with a
        // non-zero linesRead) will be obvious in muConsoleDebug instead of
        // silently producing an empty shop grid with no error dialog.
        g_ConsoleDebug->Write(MCD_NORMAL,
            L"[XShop] LoadCategroy: parsed %d category rows from %d lines (encode=%d) <%ls>",
            rowsParsed, linesRead, (int)enc, szFilePath);
    }
    else
    {
        result.SetResult(PT_LOADLIBRARY, LastError, L"package file open fail");
    }

    return result;
}

WZResult CShopList::LoadPackage(const wchar_t* szFilePath) // OK
{
    WZResult result;

    FILE_ENCODE enc = this->IsFileEncodingUtf8(szFilePath);

    std::ifstream ifs;

    ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);

    DWORD LastError = GetLastError();

    for (int n = 0; !ifs.is_open() && n < 10; ++n)
    {
        Sleep(0x64);
        ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);
        LastError = GetLastError();
    }

    char buff[1024] = { 0 };

    if (ifs.is_open())
    {
        this->GetPackageListPtr()->Clear();

        while (true)
        {
            if (!ifs.getline(buff, sizeof(buff)))
                break;

            CShopPackage pack;

            if (pack.SetPackage(this->GetDecodedString(buff, enc)))
            {
                this->GetPackageListPtr()->Append(pack);
                this->GetCategoryListPtr()->InsertPackage(pack.ProductDisplaySeq, pack.PackageProductSeq);
            }
        }

        ifs.close();
    }
    else
    {
        result.SetResult(4, LastError, L"package file open fail");
    }

    return result;
}

WZResult CShopList::LoadProduct(const wchar_t* szFilePath) // OK
{
    static WZResult result;

    result.BuildSuccessResult();

    FILE_ENCODE enc = this->IsFileEncodingUtf8(szFilePath);

    std::ifstream ifs;

    ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);

    DWORD LastError = GetLastError();

    for (int n = 0; !ifs.is_open() && n < 10; ++n)
    {
        Sleep(0x64);
        ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);
        LastError = GetLastError();
    }

    char buff[1024] = { 0 };

    if (ifs.is_open())
    {
        this->GetProductListPtr()->Clear();

        while (true)
        {
            memset(buff, 0, sizeof(buff));

            if (!ifs.getline(buff, sizeof(buff)))
                break;

            CShopProduct product;

            std::wstring data = this->GetDecodedString(buff, enc);

            if (product.SetProduct(data))
            {
                this->GetProductListPtr()->Append(product);
            }
        }

        ifs.close();
    }
    else
    {
        result.SetResult(4, LastError, L"package file open fail");
    }

    return result;
}

void CShopList::SetCategoryListPtr(CShopCategoryList* CategoryListPtr) // OK
{
    m_CategoryListPtr = CategoryListPtr;
}

void CShopList::SetPackageListPtr(CShopPackageList* PackagePtr) // OK
{
    m_PackageListPtr = PackagePtr;
}

void CShopList::SetProductListPtr(CShopProductList* ProductListPtr) // OK
{
    m_ProductListPtr = ProductListPtr;
}

FILE_ENCODE CShopList::IsFileEncodingUtf8(const wchar_t* szFilePath) // OK
{
    std::ifstream ifs;

    ifs.open(ResolveCatalogPath(szFilePath), std::ifstream::in);

    if (!ifs.is_open())
    {
        return FE_ANSI;
    }

    char buff[16] = { 0 };

    ifs.getline(buff, sizeof(buff));

    ifs.close();

    if (strlen(buff) < 3)
    {
        return FE_ANSI;
    }

    // char is signed on MSVC, so comparing it directly against 0xEF/0xBB/...
    // is always false (-17 != 239) and the BOM was never detected - Hebrew
    // UTF-8 files fell into the CP_ACP path and decoded to garbage numbers.
    const auto* ubuff = reinterpret_cast<const unsigned char*>(buff);

    if (ubuff[0] == 0xEF && ubuff[1] == 0xBB && ubuff[2] == 0xBF)
    {
        return FE_UTF8;
    }

    if (ubuff[0] == 0xFF && ubuff[1] == 0xFE)
    {
        return FE_UNICODE;
    }

    return FE_ANSI;
}

std::wstring CShopList::GetDecodedString(const char* buffer, FILE_ENCODE encode) // OK
{
    std::wstring result;

    if (encode == FE_UNICODE)
    {
        // UTF-16LE input: std::ifstream::getline read the raw little-endian bytes
        // into a narrow buffer, so they already are wide characters. Reinterpret
        // is the correct decode here (catalog files are ANSI today, so this path
        // is not normally exercised, but keep it lossless rather than empty).
        result = reinterpret_cast<const wchar_t*>(buffer);
        return result;
    }

    // FE_ANSI -> CP_ACP, FE_UTF8 -> CP_UTF8. Convert the narrow bytes to UTF-16
    // properly. The old decompiled code reinterpret-cast the narrow ASCII bytes
    // as wchar_t* ("todo: check if that's correct"), which turned a row like
    // "10@Item@200@..." into a single garbage token with no wide '@'
    // delimiters. CStringToken then yielded 1 field instead of 7, so every row
    // decoded to Root=0 -> zero category zones -> no tabs and an empty grid.
    const UINT codePage = (encode == FE_UTF8) ? CP_UTF8 : CP_ACP;

    int cchWideChar = MultiByteToWideChar(codePage, 0, buffer, -1, 0, 0);
    if (cchWideChar <= 0)
    {
        return result; // empty string on conversion failure
    }

    auto lpWideCharStr = new WCHAR[cchWideChar];
    MultiByteToWideChar(codePage, 0, buffer, -1, lpWideCharStr, cchWideChar);

    result = lpWideCharStr;

    delete[] lpWideCharStr;

    // Strip the UTF-8 BOM that rides on the FIRST line of the file: it decodes
    // to U+FEFF, glues itself to the first '@' token ("<FEFF>10"), and _wtoi
    // then yields 0 - which breaks the zone/category linkage (parent numbers
    // no longer match) and leaves the shop grid empty.
    if (!result.empty() && result[0] == 0xFEFF)
    {
        result.erase(0, 1);
    }

    return result;
}
#endif
/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "compileroptionsbuilder.h"

#include "cppmodelmanager.h"

#include <coreplugin/icore.h>
#include <coreplugin/vcsmanager.h>

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/project.h>

#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QDir>
#include <QRegularExpression>

namespace CppTools {

CompilerOptionsBuilder::CompilerOptionsBuilder(const ProjectPart &projectPart,
                                               UseSystemHeader useSystemHeader,
                                               QString clangVersion,
                                               QString clangResourceDirectory)
    : m_projectPart(projectPart)
    , m_useSystemHeader(useSystemHeader)
    , m_clangVersion(clangVersion)
    , m_clangResourceDirectory(clangResourceDirectory)
{
}

QStringList CompilerOptionsBuilder::build(CppTools::ProjectFile::Kind fileKind, PchUsage pchUsage)
{
    m_options.clear();

    if (fileKind == ProjectFile::CHeader || fileKind == ProjectFile::CSource) {
        QTC_ASSERT(m_projectPart.languageVersion <= ProjectPart::LatestCVersion,
                   return QStringList(););
    }

    add("-c");

    addWordWidth();
    addTargetTriple();
    addExtraCodeModelFlags();
    updateLanguageOption(fileKind);
    addOptionsForLanguage(/*checkForBorlandExtensions*/ true);
    enableExceptions();

    addToolchainAndProjectMacros();
    undefineClangVersionMacrosForMsvc();
    undefineCppLanguageFeatureMacrosForMsvc2015();
    addDefineFunctionMacrosMsvc();

    addGlobalUndef();
    addPrecompiledHeaderOptions(pchUsage);
    addHeaderPathOptions();
    addProjectConfigFileInclude();

    addMsvcCompatibilityVersion();

    addExtraOptions();

    insertPredefinedHeaderPathsOptions();

    return options();
}

static QStringList createLanguageOptionGcc(ProjectFile::Kind fileKind, bool objcExt)
{
    QStringList opts;

    switch (fileKind) {
    case ProjectFile::Unclassified:
    case ProjectFile::Unsupported:
        break;
    case ProjectFile::CHeader:
        if (objcExt)
            opts += QLatin1String("objective-c-header");
        else
            opts += QLatin1String("c-header");
        break;

    case ProjectFile::CXXHeader:
    default:
        if (!objcExt) {
            opts += QLatin1String("c++-header");
            break;
        }
        Q_FALLTHROUGH();
    case ProjectFile::ObjCHeader:
    case ProjectFile::ObjCXXHeader:
        opts += QLatin1String("objective-c++-header");
        break;

    case ProjectFile::CSource:
        if (!objcExt) {
            opts += QLatin1String("c");
            break;
        }
        Q_FALLTHROUGH();
    case ProjectFile::ObjCSource:
        opts += QLatin1String("objective-c");
        break;

    case ProjectFile::CXXSource:
        if (!objcExt) {
            opts += QLatin1String("c++");
            break;
        }
        Q_FALLTHROUGH();
    case ProjectFile::ObjCXXSource:
        opts += QLatin1String("objective-c++");
        break;

    case ProjectFile::OpenCLSource:
        opts += QLatin1String("cl");
        break;
    case ProjectFile::CudaSource:
        opts += QLatin1String("cuda");
        break;
    }

    if (!opts.isEmpty())
        opts.prepend(QLatin1String("-x"));

    return opts;
}

QStringList CompilerOptionsBuilder::options() const
{
    return m_options;
}

void CompilerOptionsBuilder::add(const QString &option)
{
    m_options.append(option);
}

void CompilerOptionsBuilder::addDefine(const ProjectExplorer::Macro &macro)
{
    m_options.append(defineDirectiveToDefineOption(macro));
}

void CompilerOptionsBuilder::addWordWidth()
{
    const QString argument = m_projectPart.toolChainWordWidth == ProjectPart::WordWidth64Bit
            ? QLatin1String("-m64")
            : QLatin1String("-m32");
    add(argument);
}

void CompilerOptionsBuilder::addTargetTriple()
{
    if (!m_projectPart.toolChainTargetTriple.isEmpty()) {
        m_options.append(QLatin1String("-target"));
        m_options.append(m_projectPart.toolChainTargetTriple);
    }
}

void CompilerOptionsBuilder::addExtraCodeModelFlags()
{
    // extraCodeModelFlags keep build architecture for cross-compilation.
    // In case of iOS build target triple has aarch64 archtecture set which makes
    // code model fail with CXError_Failure. To fix that we explicitly provide architecture.
    m_options.append(m_projectPart.extraCodeModelFlags);
}

void CompilerOptionsBuilder::enableExceptions()
{
    if (m_projectPart.languageVersion > ProjectPart::LatestCVersion)
        add(QLatin1String("-fcxx-exceptions"));
    add(QLatin1String("-fexceptions"));
}

void CompilerOptionsBuilder::addHeaderPathOptions()
{
    using ProjectExplorer::HeaderPathType;

    QStringList includes;
    QStringList systemIncludes;
    QStringList builtInIncludes;

    for (const ProjectExplorer::HeaderPath &headerPath : qAsConst(m_projectPart.headerPaths)) {
        if (headerPath.path.isEmpty())
            continue;

        if (excludeHeaderPath(headerPath.path))
            continue;

        switch (headerPath.type) {
        case HeaderPathType::Framework:
            includes.append("-F");
            includes.append(QDir::toNativeSeparators(headerPath.path));
            break;
        default: // This shouldn't happen, but let's be nice..:
            // intentional fall-through:
        case HeaderPathType::User:
            includes.append(includeDirOptionForPath(headerPath.path));
            includes.append(QDir::toNativeSeparators(headerPath.path));
            break;
        case HeaderPathType::BuiltIn:
            builtInIncludes.append("-isystem");
            builtInIncludes.append(QDir::toNativeSeparators(headerPath.path));
            break;
        case HeaderPathType::System:
            systemIncludes.append(m_useSystemHeader == UseSystemHeader::No
                                  ? QLatin1String("-I")
                                  : QLatin1String("-isystem"));
            systemIncludes.append(QDir::toNativeSeparators(headerPath.path));
            break;
        }
    }

    m_options.append(includes);
    m_options.append(systemIncludes);
    m_options.append(builtInIncludes);
}

void CompilerOptionsBuilder::addPrecompiledHeaderOptions(PchUsage pchUsage)
{
    if (pchUsage == PchUsage::None)
        return;

    QStringList result;

    const QString includeOptionString = includeOption();
    foreach (const QString &pchFile, m_projectPart.precompiledHeaders) {
        if (QFile::exists(pchFile)) {
            result += includeOptionString;
            result += QDir::toNativeSeparators(pchFile);
        }
    }

    m_options.append(result);
}

void CompilerOptionsBuilder::addToolchainAndProjectMacros()
{
    addMacros(m_projectPart.toolChainMacros);
    addMacros(m_projectPart.projectMacros);
}

void CompilerOptionsBuilder::addMacros(const ProjectExplorer::Macros &macros)
{
    QStringList result;

    for (const ProjectExplorer::Macro &macro : macros) {
        if (excludeDefineDirective(macro))
            continue;

        const QString defineOption = defineDirectiveToDefineOption(macro);
        if (!result.contains(defineOption))
            result.append(defineOption);
    }

    m_options.append(result);
}

void CompilerOptionsBuilder::updateLanguageOption(ProjectFile::Kind fileKind)
{
    const bool objcExt = m_projectPart.languageExtensions & ProjectPart::ObjectiveCExtensions;
    const QStringList options = createLanguageOptionGcc(fileKind, objcExt);
    if (options.isEmpty())
        return;

    QTC_ASSERT(options.size() == 2, return;);
    int langOptIndex = m_options.indexOf("-x");
    if (langOptIndex == -1) {
        m_options.append(options);
    } else {
        m_options[langOptIndex + 1] = options[1];
    }
}

void CompilerOptionsBuilder::addOptionsForLanguage(bool checkForBorlandExtensions)
{
    QStringList opts;
    const ProjectPart::LanguageExtensions languageExtensions = m_projectPart.languageExtensions;
    const bool gnuExtensions = languageExtensions & ProjectPart::GnuExtensions;

    switch (m_projectPart.languageVersion) {
    case ProjectPart::C89:
        opts << (gnuExtensions ? QLatin1String("-std=gnu89") : QLatin1String("-std=c89"));
        break;
    case ProjectPart::C99:
        opts << (gnuExtensions ? QLatin1String("-std=gnu99") : QLatin1String("-std=c99"));
        break;
    case ProjectPart::C11:
        opts << (gnuExtensions ? QLatin1String("-std=gnu11") : QLatin1String("-std=c11"));
        break;
    case ProjectPart::CXX11:
        opts << (gnuExtensions ? QLatin1String("-std=gnu++11") : QLatin1String("-std=c++11"));
        break;
    case ProjectPart::CXX98:
        opts << (gnuExtensions ? QLatin1String("-std=gnu++98") : QLatin1String("-std=c++98"));
        break;
    case ProjectPart::CXX03:
        opts << (gnuExtensions ? QLatin1String("-std=gnu++03") : QLatin1String("-std=c++03"));
        break;
    case ProjectPart::CXX14:
        opts << (gnuExtensions ? QLatin1String("-std=gnu++14") : QLatin1String("-std=c++14"));
        break;
    case ProjectPart::CXX17:
        opts << (gnuExtensions ? QLatin1String("-std=gnu++17") : QLatin1String("-std=c++17"));
        break;
    }

    if (languageExtensions & ProjectPart::MicrosoftExtensions)
        opts << QLatin1String("-fms-extensions");

    if (checkForBorlandExtensions && (languageExtensions & ProjectPart::BorlandExtensions))
        opts << QLatin1String("-fborland-extensions");

    m_options.append(opts);
}

static QByteArray toMsCompatibilityVersionFormat(const QByteArray &mscFullVer)
{
    return mscFullVer.left(2)
         + QByteArray(".")
         + mscFullVer.mid(2, 2);
}

static QByteArray msCompatibilityVersionFromDefines(const ProjectExplorer::Macros &macros)
{
    for (const ProjectExplorer::Macro &macro : macros) {
        if (macro.key == "_MSC_FULL_VER")
            return toMsCompatibilityVersionFormat(macro.value);
    }

    return QByteArray();
}

void CompilerOptionsBuilder::addMsvcCompatibilityVersion()
{
    if (m_projectPart.toolchainType == ProjectExplorer::Constants::MSVC_TOOLCHAIN_TYPEID) {
        const ProjectExplorer::Macros macros = m_projectPart.toolChainMacros + m_projectPart.projectMacros;
        const QByteArray msvcVersion = msCompatibilityVersionFromDefines(macros);

        if (!msvcVersion.isEmpty()) {
            const QString option = QLatin1String("-fms-compatibility-version=")
                    + QLatin1String(msvcVersion);
            m_options.append(option);
        }
    }
}

static QStringList languageFeatureMacros()
{
    // CLANG-UPGRADE-CHECK: Update known language features macros.
    // Collected with the following command line.
    //   * Use latest -fms-compatibility-version and -std possible.
    //   * Compatibility version 19 vs 1910 did not matter.
    //  $ clang++ -fms-compatibility-version=19 -std=c++1z -dM -E D:\empty.cpp | grep __cpp_
    static QStringList macros{
        QLatin1String("__cpp_aggregate_bases"),
        QLatin1String("__cpp_aggregate_nsdmi"),
        QLatin1String("__cpp_alias_templates"),
        QLatin1String("__cpp_aligned_new"),
        QLatin1String("__cpp_attributes"),
        QLatin1String("__cpp_binary_literals"),
        QLatin1String("__cpp_capture_star_this"),
        QLatin1String("__cpp_constexpr"),
        QLatin1String("__cpp_decltype"),
        QLatin1String("__cpp_decltype_auto"),
        QLatin1String("__cpp_deduction_guides"),
        QLatin1String("__cpp_delegating_constructors"),
        QLatin1String("__cpp_digit_separators"),
        QLatin1String("__cpp_enumerator_attributes"),
        QLatin1String("__cpp_exceptions"),
        QLatin1String("__cpp_fold_expressions"),
        QLatin1String("__cpp_generic_lambdas"),
        QLatin1String("__cpp_hex_float"),
        QLatin1String("__cpp_if_constexpr"),
        QLatin1String("__cpp_inheriting_constructors"),
        QLatin1String("__cpp_init_captures"),
        QLatin1String("__cpp_initializer_lists"),
        QLatin1String("__cpp_inline_variables"),
        QLatin1String("__cpp_lambdas"),
        QLatin1String("__cpp_namespace_attributes"),
        QLatin1String("__cpp_nested_namespace_definitions"),
        QLatin1String("__cpp_noexcept_function_type"),
        QLatin1String("__cpp_nontype_template_args"),
        QLatin1String("__cpp_nsdmi"),
        QLatin1String("__cpp_range_based_for"),
        QLatin1String("__cpp_raw_strings"),
        QLatin1String("__cpp_ref_qualifiers"),
        QLatin1String("__cpp_return_type_deduction"),
        QLatin1String("__cpp_rtti"),
        QLatin1String("__cpp_rvalue_references"),
        QLatin1String("__cpp_static_assert"),
        QLatin1String("__cpp_structured_bindings"),
        QLatin1String("__cpp_template_auto"),
        QLatin1String("__cpp_threadsafe_static_init"),
        QLatin1String("__cpp_unicode_characters"),
        QLatin1String("__cpp_unicode_literals"),
        QLatin1String("__cpp_user_defined_literals"),
        QLatin1String("__cpp_variable_templates"),
        QLatin1String("__cpp_variadic_templates"),
        QLatin1String("__cpp_variadic_using"),
    };

    return macros;
}

void CompilerOptionsBuilder::undefineCppLanguageFeatureMacrosForMsvc2015()
{
    if (m_projectPart.toolchainType == ProjectExplorer::Constants::MSVC_TOOLCHAIN_TYPEID
            && m_projectPart.isMsvc2015Toolchain) {
        // Undefine the language feature macros that are pre-defined in clang-cl,
        // but not in MSVC's cl.exe.
        foreach (const QString &macroName, languageFeatureMacros())
            m_options.append(undefineOption() + macroName);
    }
}

void CompilerOptionsBuilder::addDefineFunctionMacrosMsvc()
{
    if (m_projectPart.toolchainType == ProjectExplorer::Constants::MSVC_TOOLCHAIN_TYPEID)
        addMacros({{"__FUNCSIG__", "\"\""}, {"__FUNCTION__", "\"\""}, {"__FUNCDNAME__", "\"\""}});
}

QString CompilerOptionsBuilder::includeDirOptionForPath(const QString &path) const
{
    if (m_useSystemHeader == UseSystemHeader::No
            || path.startsWith(m_projectPart.project->rootProjectDirectory().toString())) {
        return QString("-I");
    } else {
        return QString("-isystem");
    }
}

QByteArray CompilerOptionsBuilder::macroOption(const ProjectExplorer::Macro &macro) const
{
    switch (macro.type) {
        case ProjectExplorer::MacroType::Define:     return defineOption().toUtf8();
        case ProjectExplorer::MacroType::Undefine:   return undefineOption().toUtf8();
        default: return QByteArray();
    }
}

QByteArray CompilerOptionsBuilder::toDefineOption(const ProjectExplorer::Macro &macro) const
{
    return macro.toKeyValue(macroOption(macro));
}

QString CompilerOptionsBuilder::defineDirectiveToDefineOption(const ProjectExplorer::Macro &macro) const
{
    const QByteArray option = toDefineOption(macro);

    return QString::fromUtf8(option);
}

QString CompilerOptionsBuilder::defineOption() const
{
    return QLatin1String("-D");
}

QString CompilerOptionsBuilder::undefineOption() const
{
    return QLatin1String("-U");
}

QString CompilerOptionsBuilder::includeOption() const
{
    return QLatin1String("-include");
}

bool CompilerOptionsBuilder::excludeDefineDirective(const ProjectExplorer::Macro &macro) const
{
    // Ignore for all compiler toolchains since LLVM has it's own implementation for
    // __has_include(STR) and __has_include_next(STR)
    if (macro.key.startsWith("__has_include"))
        return true;

    // If _FORTIFY_SOURCE is defined (typically in release mode), it will
    // enable the inclusion of extra headers to help catching buffer overflows
    // (e.g. wchar.h includes wchar2.h). These extra headers use
    // __builtin_va_arg_pack, which clang does not support (yet), so avoid
    // including those.
    if (m_projectPart.toolchainType == ProjectExplorer::Constants::GCC_TOOLCHAIN_TYPEID
            && macro.key == "_FORTIFY_SOURCE") {
        return true;
    }

    // MinGW 6 supports some fancy asm output flags and uses them in an
    // intrinsics header pulled in by windows.h. Clang does not know them.
    if (m_projectPart.toolchainType == ProjectExplorer::Constants::MINGW_TOOLCHAIN_TYPEID
            && macro.key == "__GCC_ASM_FLAG_OUTPUTS__") {
        return true;
    }

    return false;
}

bool CompilerOptionsBuilder::excludeHeaderPath(const QString &headerPath) const
{
    // Always exclude clang system includes (including intrinsics) which do not come with libclang
    // that Qt Creator uses for code model.
    // For example GCC on macOS uses system clang include path which makes clang code model
    // include incorrect system headers.
    static QRegularExpression clangIncludeDir(
                QLatin1String("\\A.*[\\/\\\\]lib\\d*[\\/\\\\]clang[\\/\\\\]\\d+\\.\\d+(\\.\\d+)?[\\/\\\\]include\\z"));
    return clangIncludeDir.match(headerPath).hasMatch();
}

static QString creatorResourcePath()
{
#ifndef UNIT_TESTS
    return Core::ICore::resourcePath();
#else
    return QString();
#endif
}

static QString clangIncludeDirectory(const QString &clangVersion,
                                     const QString &clangResourceDirectory)
{
#ifndef UNIT_TESTS
    return Core::ICore::clangIncludeDirectory(clangVersion, clangResourceDirectory);
#else
    return QString();
#endif
}

static int lastIncludeIndex(const QStringList &options, const QRegularExpression &includePathRegEx)
{
    int index = options.lastIndexOf(includePathRegEx);

    while (index > 0 && options[index - 1] != "-I" && options[index - 1] != "-isystem")
        index = options.lastIndexOf(includePathRegEx, index - 1);

    if (index == 0)
        index = -1;

    return index;
}

static int includeIndexForResourceDirectory(const QStringList &options)
{
    // include/c++/{version}, include/c++/v1 and include/g++
    const int cppIncludeIndex = lastIncludeIndex(
                options,
                QRegularExpression("\\A.*[\\/\\\\]include[\\/\\\\].*(g\\+\\+.*\\z|c\\+\\+[\\/\\\\](v1\\z|\\d+.*\\z))"));

    if (cppIncludeIndex > 0)
        return cppIncludeIndex + 1;

    return -1;
}

void CompilerOptionsBuilder::insertPredefinedHeaderPathsOptions()
{
    if (m_clangVersion.isEmpty())
        return;

    QStringList wrappedQtHeaders;
    addWrappedQtHeadersIncludePath(wrappedQtHeaders);

    QStringList predefinedOptions;
    predefinedOptions.append("-nostdinc");
    predefinedOptions.append("-nostdlibinc");
    addClangIncludeFolder(predefinedOptions);

    const int index = m_options.indexOf(QRegularExpression("\\A-I.*\\z"));
    if (index < 0) {
        m_options.append(wrappedQtHeaders);
        m_options.append(predefinedOptions);
        return;
    }

    int includeIndexForResourceDir = includeIndexForResourceDirectory(m_options);
    if (includeIndexForResourceDir < index)
        includeIndexForResourceDir = index;

    m_options = m_options.mid(0, index)
                + wrappedQtHeaders
                + m_options.mid(index, includeIndexForResourceDir - index)
                + predefinedOptions
                + m_options.mid(includeIndexForResourceDir);
}

void CompilerOptionsBuilder::addClangIncludeFolder(QStringList &list)
{
    QTC_CHECK(!m_clangVersion.isEmpty());
    const QString clangIncludeDir = clangIncludeDirectory(m_clangVersion, m_clangResourceDirectory);

    list.append(includeDirOptionForPath(clangIncludeDir));
    list.append(clangIncludeDir);
}

void CompilerOptionsBuilder::addWrappedQtHeadersIncludePath(QStringList &list)
{
    static const QString resourcePath = creatorResourcePath();
    static QString wrappedQtHeadersPath = resourcePath + "/cplusplus/wrappedQtHeaders";
    QTC_ASSERT(QDir(wrappedQtHeadersPath).exists(), return;);

    if (m_projectPart.qtVersion != CppTools::ProjectPart::NoQt) {
        const QString wrappedQtCoreHeaderPath = wrappedQtHeadersPath + "/QtCore";
        list.append(includeDirOptionForPath(wrappedQtHeadersPath));
        list.append(QDir::toNativeSeparators(wrappedQtHeadersPath));
        list.append(includeDirOptionForPath(wrappedQtHeadersPath));
        list.append(QDir::toNativeSeparators(wrappedQtCoreHeaderPath));
    }
}

void CompilerOptionsBuilder::addGlobalUndef()
{
    // In case of MSVC we need builtin clang defines to correctly handle clang includes
    if (m_projectPart.toolchainType != ProjectExplorer::Constants::MSVC_TOOLCHAIN_TYPEID
        && m_projectPart.toolchainType != ProjectExplorer::Constants::CLANG_CL_TOOLCHAIN_TYPEID) {
        add("-undef");
    }
}

void CompilerOptionsBuilder::addProjectConfigFileInclude()
{
    if (!m_projectPart.projectConfigFile.isEmpty()) {
        add("-include");
        add(QDir::toNativeSeparators(m_projectPart.projectConfigFile));
    }
}

void CompilerOptionsBuilder::undefineClangVersionMacrosForMsvc()
{
    if (m_projectPart.toolchainType == ProjectExplorer::Constants::MSVC_TOOLCHAIN_TYPEID) {
        static QStringList macroNames {
            "__clang__",
            "__clang_major__",
            "__clang_minor__",
            "__clang_patchlevel__",
            "__clang_version__"
        };

        foreach (const QString &macroName, macroNames)
            add(undefineOption() + macroName);
    }
}

} // namespace CppTools

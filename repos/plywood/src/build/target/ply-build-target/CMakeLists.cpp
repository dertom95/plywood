/*------------------------------------
  ///\  Plywood C++ Framework
  \\\/  https://plywood.arc80.com/
------------------------------------*/
#include <ply-build-common/Core.h>
#include <ply-build-target/CMakeLists.h>
#include <ply-build-target/BuildTarget.h>
#include <ply-runtime/algorithm/Find.h>

namespace ply {
namespace build {

#include "ply-build-target/NativeToolchain.inl"

void writeCMakeLists(StringWriter* sw, CMakeBuildFolder* cbf) {
    PLY_ASSERT(NativePath::isNormalized(cbf->absPath));
    PLY_ASSERT(NativePath::endsWithSep(cbf->absPath));

    HybridString sourceFolderPrefix = cbf->sourceFolderPrefix.view();
    if (!sourceFolderPrefix) {
        sourceFolderPrefix = NativePath::join(PLY_WORKSPACE_FOLDER, "repos/plywood/src/");
    }

    // Define lambda expression that will filter filenames when generating CMakeLists.txt in
    // bootstrap mode:
    auto filterPath = [&](StringView filePath) -> HybridString {
        PLY_ASSERT(NativePath::isNormalized(filePath));
        if (cbf->forBootstrap) {
            if (filePath.startsWith(sourceFolderPrefix)) {
                return StringView{"${SRC_FOLDER}"} +
                       PosixPath::from<NativePath>(filePath.subStr(sourceFolderPrefix.numBytes));
            } else if (filePath.startsWith(cbf->absPath)) {
                return StringView{"${BUILD_FOLDER}"} +
                       PosixPath::from<NativePath>(filePath.subStr(cbf->absPath.numBytes));
            }
        }
        return PosixPath::from<NativePath>(filePath);
    };

    // CMakeLists 3.8 supports generator expressions such as "$<CONFIG>", which is used in
    // COMPILE_FLAGS source property on Win32
    *sw << "cmake_minimum_required(VERSION 3.8)\n";
    *sw << "set(CMAKE_CONFIGURATION_TYPES \"Debug;RelWithAsserts;RelWithDebInfo\" CACHE "
           "INTERNAL "
           "\"Build configs\")\n";
    sw->format("project({})\n", cbf->solutionName);
    if (cbf->forBootstrap) {
        *sw << R"(set(WORKSPACE_FOLDER "<<<WORKSPACE_FOLDER>>>")
set(SRC_FOLDER "<<<SRC_FOLDER>>>")
set(BUILD_FOLDER "<<<BUILD_FOLDER>>>")
include("${CMAKE_CURRENT_LIST_DIR}/Helper.cmake")
)";
    } else {
        sw->format("include(\"{}\")\n",
                   fmt::EscapedString(PosixPath::from<NativePath>(NativePath::join(
                       PLY_WORKSPACE_FOLDER, "repos/plywood/scripts/Helper.cmake"))));
    }

    // Iterate over all targets
    for (const BuildTarget* buildTarget : cbf->targets) {
        PLY_ASSERT(buildTarget);
        /* FIXME
        if (buildTarget->key.dynLinkage == DynamicLinkage::Import &&
            buildTarget->sourceFilesWhenImported.isEmpty())
            continue;
        */

        StringView uniqueTargetName = buildTarget->name;
        sw->format("\n# {}\n", uniqueTargetName);

        // Define a CMake variable for each group of source files (usually, there's just one
        // group)
        Array<String> sourceVarNames;
        ArrayView<const BuildTarget::SourceFilesPair> sourceFiles = buildTarget->sourceFiles.view();
        /* FIXME
        if (buildTarget->key.dynLinkage == DynamicLinkage::Import) {
            sourceFiles = buildTarget->sourceFilesWhenImported.view();
        }
        */
        for (const BuildTarget::SourceFilesPair& sfPair : sourceFiles) {
            String varName = uniqueTargetName.upperAsc() + "_SOURCES";
            sourceVarNames.append(varName);
            sw->format("SetSourceFolders({} \"{}\"\n", varName,
                       fmt::EscapedString(filterPath(sfPair.root)));
            for (StringView relPath : sfPair.relFiles) {
                sw->format("    \"{}\"\n", fmt::EscapedString(filterPath(relPath)));
            }
            *sw << ")\n";
        }

        // Add this target
        BuildTargetType targetType = buildTarget->targetType;
        switch (targetType) {
            case BuildTargetType::HeaderOnly: {
                sw->format("add_custom_target({} SOURCES\n", uniqueTargetName);
                break;
            }
            case BuildTargetType::Lib: {
                sw->format("add_library({}\n", uniqueTargetName);
                break;
            }
            case BuildTargetType::ObjectLib: {
                // OBJECT libraries ensure that __declspec(dllexport) works correctly (eg. for
                // PLY_DLL_ENTRY). OBJECT libraries pass individual .obj files to the linker
                // instead of .lib files. If we use .lib files instead, some DLL exports may
                // get dropped if there are no references to the .obj where the export is
                // defined:
                sw->format("add_library({} OBJECT\n", uniqueTargetName);
                break;
            }
            case BuildTargetType::DLL: {
                sw->format("add_library({} SHARED\n", uniqueTargetName);
                break;
            }
            case BuildTargetType::EXE: {
                sw->format("add_executable({}\n", uniqueTargetName);
                break;
            }
        }
        for (StringView varName : sourceVarNames) {
            sw->format("    ${{{}}}\n", varName);
        }
        if (targetType == BuildTargetType::DLL || targetType == BuildTargetType::EXE) {
            // Use TARGET_OBJECTS generator expression to support OBJECT libraries
            for (s32 i = buildTarget->libs.numItems() - 1; i >= 0; i--) {
                StringView lib = buildTarget->libs[i];
                if (lib.startsWith("$<TARGET_OBJECTS")) {
                    sw->format("    {}\n", lib);
                }
            }
        }
        *sw << ")\n";
        if (targetType == BuildTargetType::EXE) {
            sw->format("set_property(TARGET {} PROPERTY ENABLE_EXPORTS TRUE)\n", uniqueTargetName);
        }

        // Precompiled headers
        if (!buildTarget->precompiledHeader.pchInclude.isEmpty()) {
            for (StringView varName : sourceVarNames) {
                sw->format("SetPrecompiledHeader({} {}\n    \"{}\"\n    \"{}\"\n    \"{}\"\n)\n",
                           uniqueTargetName, varName,
                           filterPath(buildTarget->precompiledHeader.generatorSourcePath),
                           buildTarget->precompiledHeader.pchInclude,
                           uniqueTargetName + ".$<CONFIG>.pch");
            }
        }

        // Enable/disable C++ execptions
        if (targetType != BuildTargetType::HeaderOnly) {
            bool enableExceptions =
                (findItem(buildTarget->privateAbstractFlags.view(), StringView{"exceptions"}) >= 0);
            sw->format("EnableCppExceptions({} {})\n", uniqueTargetName,
                       enableExceptions ? "TRUE" : "FALSE");
        }

        // Include directories
        if (targetType != BuildTargetType::HeaderOnly) {
            // FIXME: Add ArrayView::reversed() iterator?
            sw->format("target_include_directories({} PRIVATE\n", uniqueTargetName);
            for (s32 i = buildTarget->privateIncludeDirs.numItems() - 1; i >= 0; i--) {
                sw->format("    \"{}\"\n", filterPath(buildTarget->privateIncludeDirs[i]));
            }
            *sw << ")\n";
        }

        // Defines
        if (targetType != BuildTargetType::HeaderOnly &&
            buildTarget->privateDefines.numItems() > 0) {
            sw->format("target_compile_definitions({} PRIVATE\n", uniqueTargetName);
            for (const BuildTarget::PreprocessorDefinition& define : buildTarget->privateDefines) {
                PLY_ASSERT(define.key.findByte('=') < 0);
                PLY_ASSERT(define.value.findByte('=') < 0);
                sw->format("    \"{}={}\"\n", define.key, define.value);
            }
            *sw << ")\n";
        }

        if (targetType == BuildTargetType::DLL || targetType == BuildTargetType::EXE) {
            // Define a CMake variable for each macOS framework
            Array<String> frameworkVars;
            for (StringView fw : buildTarget->frameworks) {
                String fwVar = fw.upperAsc() + "_FRAMEWORK";
                frameworkVars.append(fwVar);
                sw->format("find_library({} {})\n", fwVar, fw);
            }

            // Link libraries
            // List in reserve order so that dependencies follow dependents
            if (buildTarget->libs.numItems() > 0 || frameworkVars.numItems() > 0) {
                sw->format("target_link_libraries({} PRIVATE\n", uniqueTargetName);
                for (s32 i = buildTarget->libs.numItems() - 1; i >= 0; i--) {
                    StringView lib = buildTarget->libs[i];
                    if (!lib.startsWith("$<TARGET_OBJECTS")) {
                        if (lib.startsWith("${")) {
                            sw->format("    {}\n", lib);
                        } else {
                            sw->format("    \"{}\"\n", filterPath(lib));
                        }
                    }
                }
                for (StringView fwVar : frameworkVars) {
                    sw->format("    ${{{}}}\n", fwVar);
                }
                *sw << ")\n";
            }

            // FIXME: SafeSEH

            // Copy DLLs
            if (buildTarget->dlls.numItems() > 0) {
                sw->format("AddDLLCopyStep({}\n", uniqueTargetName);
                for (s32 i = buildTarget->dlls.numItems() - 1; i >= 0; i--) {
                    sw->format("    \"{}\"\n", PosixPath::from<NativePath>(buildTarget->dlls[i]));
                }
                *sw << ")\n";
            }

            // In bootstrap_CMakeLists.txt, add a post-build command that copies PlyTool to the
            // workspace root.
            if (cbf->forBootstrap && buildTarget->name == "plytool") {
                *sw << "add_custom_command(TARGET plytool POST_BUILD COMMAND\n";
                *sw << "   ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:plytool> "
                       "\"${WORKSPACE_FOLDER}\")\n";
            }
        }

        // copy resource-folders via add_custom_command
        ArrayView<const BuildTarget::ResourceCopyFolders> resourceCopyFolders = buildTarget->resourceCopyFolders.view();
        for (const BuildTarget::ResourceCopyFolders& resCopyFolder : resourceCopyFolders) {
            *sw << "add_custom_command(TARGET " << uniqueTargetName << " POST_BUILD \n";
            *sw <<     "COMMAND ${CMAKE_COMMAND} -E copy_directory \n";
            *sw <<     "        " << resCopyFolder.resFolderSource << " \n";
            *sw <<     "        ${CMAKE_CURRENT_BINARY_DIR}/" << resCopyFolder.resFolderDestinationRelative << " )\n\n";

        }
    }
}

PLY_NO_INLINE Tuple<s32, String> generateCMakeProject(StringView cmakeListsFolder,
                                                      const CMakeGeneratorOptions& generatorOpts,
                                                      Functor<void(StringView)> errorCallback) {
    PLY_ASSERT(generatorOpts.isValid());
    String buildFolder = NativePath::join(cmakeListsFolder, "build");
    FSResult result = FileSystem::native()->makeDirs(buildFolder);
    if (result != FSResult::OK && result != FSResult::AlreadyExists) {
        if (errorCallback) {
            errorCallback.call(String::format("Can't create folder '{}'\n", buildFolder));
        }
        return {-1, ""};
    }
    PLY_ASSERT(!generatorOpts.generator.isEmpty());
    Array<String> args = {"..", "-G", generatorOpts.generator};
    if (generatorOpts.platform) {
        args.extend({"-A", generatorOpts.platform});
    }
    if (generatorOpts.toolset) {
        args.extend({"-T", generatorOpts.toolset});
    }
    args.extend({String::format("-DCMAKE_BUILD_TYPE={}", generatorOpts.buildType),
                 "-DCMAKE_C_COMPILER_FORCED=1", "-DCMAKE_CXX_COMPILER_FORCED=1"});
    Owned<Subprocess> sub = Subprocess::exec(PLY_CMAKE_PATH, Array<StringView>{args.view()}.view(),
                                             buildFolder, Subprocess::Output::openSeparate());
    String output = TextFormat::platformPreference()
                        .createImporter(Owned<InStream>::create(sub->readFromStdOut.borrow()))
                        ->readRemainingContents();
    s32 rc = sub->join();
    if (rc != 0) {
        if (errorCallback) {
            errorCallback.call(String::format(
                "Error generating build system using CMake for folder '{}'\n", buildFolder));
        }
    }
    return {rc, std::move(output)};
}

PLY_NO_INLINE Tuple<s32, String> buildCMakeProject(StringView cmakeListsFolder,
                                                   const CMakeGeneratorOptions& generatorOpts,
                                                   StringView buildType, bool captureOutput) {
    PLY_ASSERT(generatorOpts.isValid());
    String buildFolder = NativePath::join(cmakeListsFolder, "build");
    Subprocess::Output outputType =
        captureOutput ? Subprocess::Output::openMerged() : Subprocess::Output::inherit();
    if (!buildType) {
        buildType = generatorOpts.buildType;
    }
    Owned<Subprocess> sub = Subprocess::exec(
        PLY_CMAKE_PATH, {"--build", ".", "--config", buildType}, buildFolder, outputType);
    String output;
    if (captureOutput) {
        output = TextFormat::platformPreference()
                     .createImporter(Owned<InStream>::create(sub->readFromStdOut.borrow()))
                     ->readRemainingContents();
    }
    return {sub->join(), std::move(output)};
}

String getTargetOutputPath(const BuildTarget* buildTarget, StringView buildFolder,
                           const CMakeGeneratorOptions& cmakeOptions, StringView buildType) {
    // Note: We may eventually want to build projects without using CMake at all, but for now,
    // CMakeGeneratorOptions is a good way to get the info we need.
    bool isMultiConfig = false;
    if (cmakeOptions.generator.startsWith("Visual Studio")) {
        isMultiConfig = true;
    } else if (cmakeOptions.generator == "Xcode") {
        isMultiConfig = true;
    } else if (cmakeOptions.generator == "Unix Makefiles") {
        PLY_ASSERT(!buildType || buildType == cmakeOptions.buildType);
    } else {
        // FIXME: Make this a not-fatal warning instead, perhaps logging to some kind of
        // thread-local variable that can be set in the caller's scope.
        PLY_ASSERT(0); // Unrecognized generator
        return {};
    }

    // FIXME: The following logic assumes we're always using a native toolchain. In order to make it
    // work with cross-compilers, we'll need to pass in more information about the target platform,
    // perhaps using ToolchainInfo. (In that case, the real question will be, in general, how to
    // initialize that ToolchainInfo.)
    StringView filePrefix;
    StringView fileExtension;
    if (buildTarget->targetType == BuildTargetType::EXE) {
#if PLY_TARGET_WIN32
        fileExtension = ".exe";
#endif
    } else if (buildTarget->targetType == BuildTargetType::DLL) {
#if PLY_TARGET_WIN32
        fileExtension = ".dll";
#elif PLY_TARGET_APPLE
        filePrefix = "lib";
        fileExtension = ".dylib";
#else
        filePrefix = "lib";
        fileExtension = ".so";
#endif
    } else if (buildTarget->targetType == BuildTargetType::DLL) {
#if PLY_TARGET_WIN32
        fileExtension = ".lib";
#else
        filePrefix = "lib";
        fileExtension = ".a";
#endif
    } else {
        PLY_ASSERT(0); // Not supported
    }

    // Compose full path to the target output:
    Array<StringView> pathComponents = {buildFolder, "build"};
    if (isMultiConfig) {
        if (!buildType) {
            buildType = cmakeOptions.buildType;
        }
        pathComponents.append(buildType);
    }
    String fullName = filePrefix + buildTarget->name + fileExtension;
    pathComponents.append(fullName);
    return NativePath::format().joinAndNormalize(Array<StringView>{pathComponents.view()}.view());
}

} // namespace build
} // namespace ply

#include "codegen/CMakeLists.inl" //%%

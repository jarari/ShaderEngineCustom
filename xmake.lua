-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("ShaderEngineCL")
set_version("0.2.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("imgui", { configs = { dx11 = true, win32 = true } })

-- define targets
target("ShaderEngineCL")
    add_rules("commonlibf4.plugin", {
        name = "ShaderEngineCL",
        author = "disi",
        description = "Fallout 4 shader engine F4SE plugin",
        plugin_file_data = [[
/* F4SE plugin metadata is defined manually in src/main.cpp. */
]]
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    add_packages("imgui")
    add_syslinks("d3d11", "d3dcompiler", "dxgi")
    set_pcxxheader("src/PCH.h")

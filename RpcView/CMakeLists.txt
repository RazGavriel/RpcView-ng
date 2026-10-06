cmake_minimum_required(VERSION 3.21)

message("[RpcView]")

project(RpcView)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
    
add_executable(
	RpcView
    
    "${PROJECT_BINARY_DIR}/../RpcViewVersion.h"
    DecompilationWidget.cpp
    EndpointsWidget.cpp
    FilterWidget.cpp
    IdlHighlighter.cpp
    InterfaceInfoWidget.cpp
    InterfacesWidget.cpp
    MainWindow.cpp
    ../RpcCommon/Misc.c
    ../RpcCommon/ntdll.c
    Pdb.c
    ProceduresWidget.cpp
    ProcessInfoWidget.cpp
    ProcessWidget.cpp
    
    ConfigurationVisitor.cpp
    EndpointSelectedVisitor.cpp
    InitViewsVisitor.cpp
    InterfaceSelectedVisitor.cpp
    ProcessSelectedVisitor.cpp
    RefreshVisitor.cpp

    RpcCoreManager.c
	RpcView.cpp
    RpcViewResource.rc
    )

# Disable compiler warnings:
# C4091 in ShlObj.h and Dbghelp.h
# C4127	(conditional expression is constant) in qt headers
set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "/wd4091 /wd4127 /wd4714 /wd4996")
target_link_libraries(RpcView PRIVATE Qt6::Widgets)
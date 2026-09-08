# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "melonDS OpenGL adaptation requires INPUT and OUTPUT")
endif()

file(READ "${INPUT}" source)
# Git's text checkout may use CRLF. Verify the pinned source content after
# normalizing line endings so the adaptation remains reproducible on Windows.
string(REPLACE "\r\n" "\n" source "${source}")
string(SHA256 actual_sha256 "${source}")
set(expected_sha256
    "3f77171742446c39a9d10ce9a38af2e0910fedbcbe34116fa9ff702cccb75eb3")
if(NOT actual_sha256 STREQUAL expected_sha256)
    message(FATAL_ERROR
        "Pinned melonDS GPU2D_OpenGL.cpp changed: expected ${expected_sha256}, "
        "got ${actual_sha256}. Audit and update the DualBoy adaptation.")
endif()

set(constructor_before [=[
GLRenderer2D::GLRenderer2D(melonDS::GPU2D& gpu2D, GLRenderer& parent)
    : Renderer2D(gpu2D), Parent(parent)
{
]=])
set(constructor_after [=[
GLRenderer2D::GLRenderer2D(melonDS::GPU2D& gpu2D, GLRenderer& parent)
    : Renderer2D(gpu2D), Parent(parent)
{
    SpritePreVtxData = nullptr;
    SpriteVtxData = nullptr;
]=])
set(destructor_before [=[
GLRenderer2D::~GLRenderer2D()
{
]=])
set(destructor_after [=[
GLRenderer2D::~GLRenderer2D()
{
    delete[] SpritePreVtxData;
    delete[] SpriteVtxData;

]=])

foreach(anchor IN ITEMS constructor destructor)
    string(LENGTH "${${anchor}_before}" source_anchor_length)
    string(REPLACE "${${anchor}_before}" "" without_anchor "${source}")
    string(LENGTH "${source}" source_length)
    string(LENGTH "${without_anchor}" without_anchor_length)
    math(EXPR removed_length "${source_length} - ${without_anchor_length}")
    if(NOT removed_length EQUAL source_anchor_length)
        message(FATAL_ERROR
            "Pinned melonDS ${anchor} adaptation anchor did not occur exactly once")
    endif()
    string(REPLACE "${${anchor}_before}" "${${anchor}_after}" source "${source}")
endforeach()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
set(write_output TRUE)
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" existing_output)
    if(existing_output STREQUAL source)
        set(write_output FALSE)
    endif()
endif()
if(write_output)
    file(WRITE "${OUTPUT}" "${source}")
endif()

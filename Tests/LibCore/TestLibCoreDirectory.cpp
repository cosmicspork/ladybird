/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>

static ErrorOr<Core::Directory> create_temp_directory()
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-libcore-directory-{}", Core::System::getpid()));
    return Core::Directory::create(path, Core::Directory::CreateDirectories::Yes);
}

TEST_CASE(directory_open_and_stat)
{
    constexpr auto text = "Hello from Core::Directory!"sv;
    ByteString directory_path;

    {
        auto directory = TRY_OR_FAIL(create_temp_directory());
        directory_path = directory.path().string();

        {
            auto file = TRY_OR_FAIL(directory.open("test-file.txt"sv, Core::File::OpenMode::Write));
            TRY_OR_FAIL(file->write_until_depleted(text.bytes()));
        }

        {
            auto file = TRY_OR_FAIL(directory.open("test-file.txt"sv, Core::File::OpenMode::Read));
            auto contents = TRY_OR_FAIL(file->read_until_eof());
            EXPECT_EQ(StringView { contents.bytes() }, text);
        }

        auto file_stat = TRY_OR_FAIL(directory.stat("test-file.txt"sv));
        EXPECT(S_ISREG(file_stat.st_mode));
        EXPECT_EQ(static_cast<size_t>(file_stat.st_size), text.length());

        auto directory_stat = TRY_OR_FAIL(directory.stat());
        EXPECT(S_ISDIR(directory_stat.st_mode));

        EXPECT(directory.stat("nonexistent-file.txt"sv).is_error());
        EXPECT(directory.open("nonexistent-file.txt"sv, Core::File::OpenMode::Read).is_error());

#ifndef AK_OS_WINDOWS
        auto private_file = TRY_OR_FAIL(directory.open("private-file.txt"sv, Core::File::OpenMode::Write, 0600));
        auto private_file_stat = TRY_OR_FAIL(private_file->stat());
        EXPECT_EQ(private_file_stat.st_mode & 0777u, 0600u);

        auto link_path = directory.path().append("test-file-link.txt"sv);
        TRY_OR_FAIL(Core::System::symlink("test-file.txt"sv, link_path.string()));
        EXPECT(directory.open("test-file-link.txt"sv, Core::File::OpenMode::Write | Core::File::OpenMode::NoFollow).is_error());

        auto file = TRY_OR_FAIL(directory.open("test-file.txt"sv, Core::File::OpenMode::ReadWrite | Core::File::OpenMode::NoFollow));
        auto contents = TRY_OR_FAIL(file->read_until_eof());
        EXPECT_EQ(StringView { contents.bytes() }, text);
#endif
    }

    TRY_OR_FAIL(FileSystem::remove(directory_path, FileSystem::RecursionMode::Allowed));
}

#ifndef AK_OS_WINDOWS
TEST_CASE(recursive_remove_does_not_follow_directory_links)
{
    auto outside = TRY_OR_FAIL(create_temp_directory());
    auto outside_path = outside.path().string();
    auto kept_path = outside.path().append("kept.txt"sv).string();
    (void)TRY_OR_FAIL(Core::File::open(kept_path, Core::File::OpenMode::Write));

    auto removed_path = ByteString::formatted("{}-removed", outside_path);
    TRY_OR_FAIL(Core::System::mkdir(removed_path, 0700));
    TRY_OR_FAIL(Core::System::symlink(outside_path, ByteString::formatted("{}/link", removed_path)));

    TRY_OR_FAIL(FileSystem::remove(removed_path, FileSystem::RecursionMode::Allowed));
    EXPECT(!FileSystem::exists(removed_path));
    EXPECT(FileSystem::exists(kept_path));

    TRY_OR_FAIL(FileSystem::remove(outside_path, FileSystem::RecursionMode::Allowed));
}
#endif

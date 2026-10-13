#include "check.hpp"

#include "zar/policy.hpp"

using zar::is_precompressed;

TEST(precompressed_extensions)
{
    CHECK(is_precompressed("photo.jpg"));
    CHECK(is_precompressed("PHOTO.JPEG"));
    CHECK(is_precompressed("music/song.m4a"));
    CHECK(is_precompressed("backup.tar.gz"));
    CHECK(is_precompressed("fonts/x.woff2"));
    CHECK(is_precompressed("nested.zar"));
}

TEST(compressible_names)
{
    CHECK(!is_precompressed("main.cpp"));
    CHECK(!is_precompressed("archive.tar"));
    CHECK(!is_precompressed("README"));
    CHECK(!is_precompressed(".zip"));               // a dotfile, not an extension
    CHECK(!is_precompressed("dir.jpg/notes"));      // the extension belongs to a directory
    CHECK(!is_precompressed("trailing."));
    CHECK(!is_precompressed("x.averyveryverylongextension"));
}

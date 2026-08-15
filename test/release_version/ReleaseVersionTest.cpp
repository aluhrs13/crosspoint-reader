#include <gtest/gtest.h>

#include "ReleaseVersion.h"

TEST(ReleaseVersion, ComparesCoreVersions) {
  EXPECT_TRUE(ReleaseVersion::isNewer("1.6.0", "1.5.9"));
  EXPECT_TRUE(ReleaseVersion::isNewer("2.0.0", "1.99.99"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.0", "1.5.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.4.9", "1.5.0"));
}

TEST(ReleaseVersion, AcceptsGitTagPrefix) {
  EXPECT_TRUE(ReleaseVersion::isNewer("v1.5.1", "1.5.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("v1.5.0", "1.5.0"));
}

TEST(ReleaseVersion, ComparesPrereleases) {
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.0", "1.5.0-rc.1"));
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.0-rc.2", "1.5.0-rc.1"));
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.0-reader.2", "1.5.0-reader.1"));
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.1", "1.5.0-dev-copilot-feature-abc1234"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.0-rc.1", "1.5.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.0-reader.1", "1.5.0-reader.1"));
}

TEST(ReleaseVersion, IgnoresBuildMetadata) {
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.0+new", "1.5.0+old"));
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.1+build.7", "1.5.0+build.9"));
}

TEST(ReleaseVersion, RejectsMalformedVersions) {
  EXPECT_FALSE(ReleaseVersion::isNewer("", "1.5.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("latest", "1.5.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5", "1.4.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.1", "dev"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.1-01", "1.5.0"));
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.1+", "1.5.0"));
}

TEST(ReleaseVersion, VariantBuildMetadataDoesNotReofferSameRelease) {
  // Firmware variants tag themselves with build metadata ("+readwise"), which
  // is ignored in precedence, so the matching release is not seen as newer.
  EXPECT_FALSE(ReleaseVersion::isNewer("1.5.0", "1.5.0+readwise"));
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.1", "1.5.0+readwise"));

  // A "-readwise" PRERELEASE suffix would rank below the same tag, making the
  // device re-offer the identical release forever. Guard against that choice.
  EXPECT_TRUE(ReleaseVersion::isNewer("1.5.0", "1.5.0-readwise"));
}

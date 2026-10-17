#include "test_support.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

using namespace rfmod;
using namespace rfmod::test;

namespace {

const Version kHost(0, 1, 0);

std::string rawTar(const std::vector<std::pair<std::string, std::string>>& entries, bool symlink = false) {
    archive* a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    std::vector<char> buf(1 << 20);
    std::size_t used = 0;
    archive_write_open_memory(a, buf.data(), buf.size(), &used);
    for (const auto& [path, content] : entries) {
        archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, path.c_str());
        if (symlink) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_symlink(e, "/etc/passwd");
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_size(e, static_cast<la_int64_t>(content.size()));
        }
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        if (!symlink) archive_write_data(a, content.data(), content.size());
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return std::string(buf.data(), used);
}

std::string failedCheck(const ValidationReport& r) {
    for (const auto& c : r.checks()) {
        if (!c.ok) return c.name;
    }
    return "";
}

}  // namespace

TEST(Crypto, KeysRoundTripAndSign) {
    const SecretKey sk = SecretKey::generate();
    const PublicKey pk = PublicKey::fromText(sk.publicKey().toText());
    EXPECT_EQ(pk, sk.publicKey());
    EXPECT_EQ(pk.id().size(), 16u);
    const Bytes sig = sk.sign("hello");
    EXPECT_TRUE(pk.verify("hello", sig));
    EXPECT_FALSE(pk.verify("hellO", sig));
    EXPECT_EQ(SecretKey::fromText(sk.toText()).publicKey(), pk);
    EXPECT_THROW((void)PublicKey::fromText("rfmod-ed25519-pub:AAAA"), std::invalid_argument);
    // Dev key pair from the repository matches itself.
    EXPECT_EQ(ownerSecret().publicKey(), PublicKey::fromText(ownerPublicText()));
}

TEST(Package, SignedPackageVerifies) {
    const TrustStore trust(ownerPublicText(), {});
    const std::string pkg = PackageWriter::pack(packageWith(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA), ownerSecret());
    ValidationReport r;
    const auto v = PackageVerifier::verify(pkg, trust, kHost, r);
    ASSERT_TRUE(v) << r.toJson().dump(2);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(v->manifest.id, "test.alpha");
    EXPECT_TRUE(v->signer.firstParty());
    // Packing is reproducible.
    EXPECT_EQ(pkg, PackageWriter::pack(packageWith(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA), ownerSecret()));
}

TEST(Package, UntrustedKeyIsRefused) {
    const TrustStore trust(ownerPublicText(), {});
    const std::string pkg = PackageWriter::pack(packageWith(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA),
                                                SecretKey::generate());
    ValidationReport r;
    EXPECT_FALSE(PackageVerifier::verify(pkg, trust, kHost, r));
    EXPECT_EQ(failedCheck(r), "signature");
}

TEST(Package, ThirdPartyKeyIsNotFirstParty) {
    const auto path = std::filesystem::temp_directory_path() / "rfmod-trust-test.json";
    std::filesystem::remove(path);
    {
        TrustStore trust(ownerPublicText(), path);
        trust.add(thirdPartySecret().publicKey(), "Test vendor");
        EXPECT_THROW(trust.remove(ownerSecret().publicKey().id()), std::invalid_argument);
    }
    const TrustStore reloaded(ownerPublicText(), path);  // admin keys persist
    ASSERT_EQ(reloaded.keys().size(), 2u);
    const std::string pkg = PackageWriter::pack(packageWith(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA),
                                                thirdPartySecret());
    ValidationReport r;
    const auto v = PackageVerifier::verify(pkg, reloaded, kHost, r);
    ASSERT_TRUE(v);
    EXPECT_FALSE(v->signer.firstParty());
    std::filesystem::remove(path);
}

TEST(Package, TamperedFileIsRefused) {
    const TrustStore trust(ownerPublicText(), {});
    PackageFiles files = packageWith(baseManifest("test.alpha", "1.0.0"), RFMOD_LIB_ALPHA);
    const std::string signedPkg = PackageWriter::pack(files, ownerSecret());
    ValidationReport read;
    PackageFiles unpacked = *PackageReader::read(signedPkg, {}, read);

    // Change one byte of the library, keep the original signature and list.
    unpacked.files["lib/libmod.so"][100] ^= 1;
    std::vector<std::pair<std::string, std::string>> entries(unpacked.files.begin(), unpacked.files.end());
    ValidationReport r;
    EXPECT_FALSE(PackageVerifier::verify(rawTar(entries), trust, kHost, r));
    EXPECT_EQ(failedCheck(r), "files.integrity");

    // Add a file the signature does not cover.
    unpacked = *PackageReader::read(signedPkg, {}, read);
    unpacked.files["lib/extra.so"] = "x";
    entries.assign(unpacked.files.begin(), unpacked.files.end());
    ValidationReport r2;
    EXPECT_FALSE(PackageVerifier::verify(rawTar(entries), trust, kHost, r2));
    EXPECT_EQ(failedCheck(r2), "files.integrity");
}

TEST(Package, UnsafeArchivesAreRefused) {
    ValidationReport r1;
    EXPECT_FALSE(PackageReader::read(rawTar({{"../evil", "x"}}), {}, r1));
    EXPECT_EQ(failedCheck(r1), "archive.paths");
    ValidationReport r2;
    EXPECT_FALSE(PackageReader::read(rawTar({{"/etc/evil", "x"}}), {}, r2));
    ValidationReport r3;
    EXPECT_FALSE(PackageReader::read(rawTar({{"lib/link", ""}}, true), {}, r3));
    EXPECT_EQ(failedCheck(r3), "archive.entries");
    ValidationReport r4;
    PackageLimits tiny;
    tiny.maxUnpackedBytes = 10;
    EXPECT_FALSE(PackageReader::read(rawTar({{"a", std::string(100, 'x')}}), tiny, r4));
    EXPECT_EQ(failedCheck(r4), "archive.size");
    ValidationReport r5;
    EXPECT_FALSE(PackageReader::read("not an archive", {}, r5));
}

TEST(Package, MissingReferencedFileIsRefused) {
    const TrustStore trust(ownerPublicText(), {});
    auto m = baseManifest("test.alpha", "1.0.0");
    m["optionsSchema"] = "schema/options.json";
    const std::string pkg = PackageWriter::pack(packageWith(m, RFMOD_LIB_ALPHA), ownerSecret());
    ValidationReport r;
    EXPECT_FALSE(PackageVerifier::verify(pkg, trust, kHost, r));
    EXPECT_EQ(failedCheck(r), "files.referenced");
}

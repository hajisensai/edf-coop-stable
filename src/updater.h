// Keeps the plugin current without anyone having to reinstall it.
//
// Every player in a room should run the same version (a fix often needs everyone), and friends do not
// follow releases. So at startup one background request asks GitHub for this repository's latest
// release; when it is newer, the plugin downloads the release's EDF6DirectNet.dll and its signed
// manifest EDF6DirectNet.dll.sig, and installs the DLL (it runs the next time the game starts).
//
// What is trusted: a public key compiled into this DLL, not GitHub. The manifest names a version and
// the SHA-256 of the DLL and is signed (ECDSA P-256) with a private key that only the release workflow
// holds. A download is taken only when the signature verifies, the signed version is the release being
// installed and newer than the running one (no downgrade), and the DLL has the signed digest and says
// it is that version. Someone who can change release assets or the connection but not sign cannot get
// a DLL installed; a release without a valid signature is rejected. Downloads also only come from this
// repository's release download URLs.
//
// A bad release must not strand anyone: the version that is replaced stays as EDF6DirectNet.dll.old
// until the new one has run for a while (kHealthySeconds past the game's first EOS tick). When a new version's previous run ended
// before that, the next start puts the old version back, remembers the new one as bad (not installed
// again) and runs this session without the plugin. There is never a moment without a loadable
// EDF6DirectNet.dll: files are swapped by renaming a complete file over the name (see swapIn).
//
// Every failure is one log line; the game never waits for the network. [Update] AutoUpdate=0 turns
// downloading off (rollback still works).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dn {

struct Version {
    int major = -1, minor = -1, patch = -1;
    bool valid() const { return major >= 0; }
    bool newerThan(const Version& other) const;
    bool operator==(const Version& other) const = default;
};

// The first x.y.z in `text` ("v0.3.6", "EDF6DirectNet 0.3.6"); invalid when there is none.
Version parseVersion(const std::string& text);
// "0.3.6"
std::string versionText(const Version& v);

// The string value of the first `"field": "..."` at or after `from` (no escapes); "" when absent.
std::string jsonString(const std::string& json, const std::string& field, size_t from = 0);
// The browser_download_url of the release asset called `name`, only when it is exactly this
// repository's download URL for the release's own tag; "" otherwise.
std::string assetUrl(const std::string& releaseJson, const std::string& name);

std::string sha256Hex(const std::vector<uint8_t>& data);
// The marker every build carries, e.g. "EDF6DN_VERSION=0.3.6".
std::string versionMarker(const Version& v);
// The digest part of a check: `dll` has the digest in `shaText` ("<hex>  name"), is a DLL, and carries
// the version marker of `version`. `why` says what failed.
bool verifyUpdate(const std::vector<uint8_t>& dll, const std::string& shaText, const Version& version, std::string* why);

// The public key releases are signed with (BCRYPT_ECCPUBLIC_BLOB, P-256). Production always uses it.
const std::vector<uint8_t>& releaseSigningKey();

// EDF6DirectNet.dll.sig, exactly three LF-terminated lines and nothing else:
//   EDF6DirectNet <x.y.z>          the version, canonical (no leading zeros)
//   <sha256 of the DLL>            64 lowercase hex digits
//   <signature>                    128 lowercase hex digits: ECDSA P-256 r||s (IEEE P1363) over the
//                                  SHA-256 of the first two lines including their LFs (the manifest)
struct SignedManifest {
    Version version;
    std::string sha256;
};
// Parses `sigFile` strictly and verifies its signature with `publicKey`.
bool readSignedManifest(const std::string& sigFile, const std::vector<uint8_t>& publicKey, SignedManifest* out,
                        std::string* why);
// Everything a download must pass before it is installed: a valid signature by `publicKey`, the signed
// version equal to `release` (the tag being installed) and newer than `current`, then verifyUpdate
// against the signed digest.
bool verifyRelease(const std::vector<uint8_t>& dll, const std::string& sigFile, const Version& release,
                   const Version& current, const std::vector<uint8_t>& publicKey, std::string* why);

// Makes `replacement` the file called `target` and keeps what was there as `aside`, with `target`
// naming a complete file at every moment: `aside` becomes a second name (hard link) of the current
// file, then `replacement` is renamed over `target`. Works while `target` is a loaded DLL (a mapped
// image cannot be overwritten or deleted, but a name of it can be replaced while another name
// remains). Without hard links (FAT/exFAT) it falls back to ReplaceFileW, which is not atomic.
bool swapIn(const std::wstring& target, const std::wstring& replacement, const std::wstring& aside, std::string* why);

// Puts `data` in place of `installed`, which may be loaded, keeping the current file as
// installed.old for rollback (see beginRun). The running version gives up its own trial: from now on
// the rollback target is itself.
bool installOver(const std::wstring& installed, const std::vector<uint8_t>& data, std::string* why);
// Deletes what earlier updates left behind and nothing needs: installed.new<pid> downloads of games that
// quit mid-install, and installed.rolledback once no game has it loaded. Part of beginRun.
void removeUpdateLeftovers(const std::wstring& installed);

// How long a new version has to run once the game is up (its first EOS tick, the title screen) before it
// counts as working. Counted from there, not from load, so quitting at the title screen is not a failure.
constexpr unsigned kHealthySeconds = 20;

enum class RunState {
    Normal,      // nothing to prove
    Trial,       // first runs of an update: call confirmHealthy after kHealthySeconds
    RolledBack,  // this version failed before: the previous one runs from the next start; do not run
};
// Called once at load, before anything else, with the files the updater keeps next to `installed`:
//   .old         the previous version, kept until the new one is healthy
//   .trial       "<version> <pid>": this version started and has not been healthy yet
//   .bad         a version that was rolled back; the updater does not install it again
//   .rolledback  the rolled-back DLL, moved aside (deleted at a later start)
//   .new<pid>    downloads a game that quit mid-install left behind (deleted)
RunState beginRun(const std::wstring& installed, const std::string& version);
// This version has run healthily: clears its trial, then deletes installed.old. Does nothing when the
// trial is not this version's (another update has been installed meanwhile).
void confirmHealthy(const std::wstring& installed, const std::string& version);
// Calls confirmHealthy on a background thread kHealthySeconds after noteGameRunning.
void startHealthWatch(const std::wstring& installed, const char* version);
// The game is up: called on every EOS tick (cheap after the first), and at load when there is no game
// for the plugin to run in (then nothing can fail and the trial ends kHealthySeconds later).
void noteGameRunning();
// The version recorded in installed.bad, invalid when none.
Version badVersion(const std::wstring& installed);

// Checks once and installs a newer release over `installed` (currently at `current`); blocks on the
// network. Returns the log line saying what happened.
std::string updateOnce(const std::wstring& installed, const std::string& current);
// Runs updateOnce on a background thread and logs its result.
void startAutoUpdate(const std::wstring& installed, const char* current);

}  // namespace dn

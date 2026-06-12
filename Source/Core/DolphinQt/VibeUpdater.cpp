// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/VibeUpdater.h"

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <picojson.h>

#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include "Common/HttpRequest.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"

namespace VibeUpdate
{
namespace
{
// The fork's release coordinates. A single fixed repo, so these are constants rather than
// build-time-generated (there is nothing to repoint).
constexpr char kReleasesApi[] = "https://api.github.com/repos/robogears/VibeDolphin/releases/latest";
constexpr char kReleasesPage[] = "https://github.com/robogears/VibeDolphin/releases";

std::string UserAgent()
{
  return "VibeDolphin/" + CurrentVersion() + " (+https://github.com/robogears/VibeDolphin)";
}

// Parse a dotted version into integer components, tolerating a single leading 'v'/'V' and
// non-numeric junk (which becomes 0). "v0.1.14" -> {0,1,14}.
std::vector<int> ParseVersion(std::string v)
{
  if (!v.empty() && (v.front() == 'v' || v.front() == 'V'))
    v.erase(0, 1);
  std::vector<int> parts;
  std::stringstream ss(v);
  std::string seg;
  while (std::getline(ss, seg, '.'))
  {
    int n = 0;
    const auto [ptr, ec] = std::from_chars(seg.data(), seg.data() + seg.size(), n);
    parts.push_back(ec == std::errc{} ? n : 0);
  }
  return parts;
}

// True iff |remote| is strictly newer than |current| (component-wise, zero-padding the shorter).
bool IsNewerVersion(const std::string& remote, const std::string& current)
{
  const auto r = ParseVersion(remote);
  const auto c = ParseVersion(current);
  for (std::size_t i = 0, n = std::max(r.size(), c.size()); i < n; ++i)
  {
    const int a = i < r.size() ? r[i] : 0;
    const int b = i < c.size() ? c[i] : 0;
    if (a != b)
      return a > b;
  }
  return false;  // equal -> not newer
}

#ifdef __linux__
QString AppImagePath()
{
  const char* const appimage = std::getenv("APPIMAGE");
  return (appimage && *appimage) ? QString::fromUtf8(appimage) : QString{};
}
#endif

// Download |url| to memory with a cancelable progress dialog. nullopt on failure/cancel.
std::optional<std::vector<u8>> DownloadWithProgress(const std::string& url, QWidget* parent)
{
  QProgressDialog progress(QObject::tr("Downloading update…"), QObject::tr("Cancel"), 0, 100, parent);
  progress.setWindowModality(Qt::WindowModal);
  progress.setMinimumDuration(0);
  progress.setAutoClose(false);
  progress.setValue(0);

  // The progress callback runs on THIS (UI) thread during the blocking Get; pump the event loop so
  // the dialog stays responsive and Cancel works. Returning false aborts the transfer.
  Common::HttpRequest http{std::chrono::seconds{300},
                           [&](s64 dltotal, s64 dlnow, s64, s64) {
                             if (dltotal > 0)
                               progress.setValue(static_cast<int>((dlnow * 100) / dltotal));
                             QApplication::processEvents();
                             return !progress.wasCanceled();
                           }};
  http.FollowRedirects(8);  // github.com -> release CDN
  const Common::HttpRequest::Headers headers = {{"User-Agent", UserAgent()}};
  Common::HttpRequest::Response data = http.Get(url, headers);
  const bool canceled = progress.wasCanceled();
  progress.close();

  if (canceled)
    return std::nullopt;
  if (!data)
    WARN_LOG_FMT(COMMON, "VibeUpdate: download of {} failed", url);
  return data;
}

// Stage the verified new AppImage and launch a detached helper that swaps it in once we exit, then
// quit so the helper can proceed. Untrusted paths are passed as argv ($1..$4), never interpolated
// into the script body, so a path with shell metacharacters can't inject commands. Returns true iff
// the helper was launched and QApplication::quit() was called (app is exiting to be relaunched);
// false on any failure or user cancel, in which case the app keeps running unchanged.
bool SelfInstallAppImage(const Update& update, QWidget* parent)
{
#ifdef __linux__
  const QString appimage = AppImagePath();

  // Stage the new image in a temp dir BESIDE the running AppImage (not the system /tmp, which on the
  // Steam Deck is usually a different filesystem). Same-filesystem staging makes the final swap an
  // atomic rename instead of a cross-device copy that, if interrupted, could leave a half-written
  // binary in place of the app.
  QTemporaryDir staging(QFileInfo(appimage).absolutePath() +
                        QStringLiteral("/.vibedolphin-update-XXXXXX"));
  if (!staging.isValid())
  {
    QMessageBox::critical(parent, QObject::tr("Update failed"),
                          QObject::tr("Could not create a temporary directory for the update."));
    return false;
  }
  staging.setAutoRemove(false);  // the detached helper removes the staging dir after applying
  const QString staging_path = staging.path();
  const QString new_image = staging_path + QStringLiteral("/VibeDolphin.AppImage");
  const QString script_path = staging_path + QStringLiteral("/apply-update.sh");

  const auto cleanup = [&] { QDir(staging_path).removeRecursively(); };

  // (1) Download.
  const auto data = DownloadWithProgress(update.asset_url, parent);
  if (!data)
  {
    cleanup();
    return false;  // user canceled, or a network failure already logged
  }

  // (2) Verify SHA-256 against the release's per-asset digest BEFORE the bytes are made executable.
  //     If the release advertised no digest, fall back to a confirmation (the file came over HTTPS
  //     from GitHub, so TLS is still a trust anchor) rather than blocking the user's own release.
  if (!update.asset_sha256.empty())
  {
    const QByteArray actual =
        QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(data->data()),
                                            static_cast<qsizetype>(data->size())),
                                 QCryptographicHash::Sha256)
            .toHex();
    if (actual.compare(QByteArray::fromStdString(update.asset_sha256), Qt::CaseInsensitive) != 0)
    {
      ERROR_LOG_FMT(COMMON, "VibeUpdate: SHA-256 mismatch (expected {})", update.asset_sha256);
      QMessageBox::critical(
          parent, QObject::tr("Update verification failed"),
          QObject::tr("The downloaded update did not match its expected checksum and was discarded."));
      cleanup();
      return false;
    }
  }
  else if (QMessageBox::warning(
               parent, QObject::tr("Update not verifiable"),
               QObject::tr("This release does not provide a checksum, so the download can't be "
                           "verified. Install it anyway?"),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
  {
    cleanup();
    return false;
  }

  // (3) Write the staged image.
  {
    QFile out(new_image);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        out.write(reinterpret_cast<const char*>(data->data()),
                  static_cast<qint64>(data->size())) != static_cast<qint64>(data->size()))
    {
      QMessageBox::critical(parent, QObject::tr("Update failed"),
                            QObject::tr("Could not write the downloaded update to disk."));
      cleanup();
      return false;
    }
  }

  // (4) Write the swap helper. It waits for THIS process to exit, atomically replaces the running
  //     AppImage, relaunches it, and removes the staging dir. Always relaunches so the user is
  //     never left with a vanished app; aborts untouched if we don't exit within the wait window.
  const QString script = QStringLiteral(
      "#!/bin/bash\n"
      "NEW=\"$1\"; TARGET=\"$2\"; PID=\"$3\"; DIR=\"$4\"\n"
      "for i in $(seq 1 60); do kill -0 \"$PID\" 2>/dev/null || break; sleep 0.5; done\n"
      "if kill -0 \"$PID\" 2>/dev/null; then rm -rf \"$DIR\"; exit 0; fi\n"
      "chmod +x \"$NEW\" 2>/dev/null\n"
      "mv -f \"$NEW\" \"$TARGET\" 2>/dev/null && chmod +x \"$TARGET\" 2>/dev/null\n"
      "nohup \"$TARGET\" >/dev/null 2>&1 &\n"
      "disown\n"
      "rm -rf \"$DIR\"\n");
  {
    QFile f(script_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        f.write(script.toUtf8()) != script.toUtf8().size())
    {
      QMessageBox::critical(parent, QObject::tr("Update failed"),
                            QObject::tr("Could not write the update helper."));
      cleanup();
      return false;
    }
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  }

  // (5) Launch the helper detached and quit. argv order matches $1..$4: NEW, TARGET, PID, DIR.
  const QString pid = QString::number(QCoreApplication::applicationPid());
  if (!QProcess::startDetached(QStringLiteral("/bin/bash"),
                               {script_path, new_image, appimage, pid, staging_path}))
  {
    QMessageBox::critical(parent, QObject::tr("Update failed"),
                          QObject::tr("Could not start the update helper."));
    cleanup();
    return false;
  }
  QApplication::quit();  // exit so the helper's wait loop unblocks -> swap + relaunch
  return true;
#else
  (void)update;
  (void)parent;
  return false;
#endif
}
}  // namespace

std::string CurrentVersion()
{
  // The branded version string is "VibeDolphin X.Y.Z"; take the last whitespace-delimited token.
  const std::string& s = Common::GetScmRevStr();
  const auto pos = s.find_last_of(' ');
  return pos == std::string::npos ? s : s.substr(pos + 1);
}

bool CanSelfInstall()
{
#ifdef __linux__
  const QString appimage = AppImagePath();
  if (appimage.isEmpty())
    return false;
  // Replacing the AppImage means creating a staging file beside it and renaming over it -- both
  // operations need write permission on the PARENT DIRECTORY (a file can be writable while its dir
  // is read-only, and vice versa), so that's the real gate. Falls back to the release page if not.
  return QFileInfo(QFileInfo(appimage).absolutePath()).isWritable();
#else
  return false;
#endif
}

Result CheckForUpdate()
{
  Common::HttpRequest http{std::chrono::seconds{10}};
  http.FollowRedirects(4);
  const Common::HttpRequest::Headers headers = {
      {"User-Agent", UserAgent()},
      {"Accept", "application/vnd.github+json"},
  };
  const Common::HttpRequest::Response body = http.Get(kReleasesApi, headers);
  if (!body)
    return {CheckStatus::CheckFailed, std::nullopt};  // network/HTTP failure -- NOT "up to date"

  picojson::value root;
  const std::string text(body->begin(), body->end());
  const std::string err = picojson::parse(root, text);
  if (!err.empty() || !root.is<picojson::object>())
    return {CheckStatus::CheckFailed, std::nullopt};
  const picojson::object& obj = root.get<picojson::object>();

  const std::string tag = ReadStringFromJson(obj, "tag_name").value_or(std::string{});
  if (tag.empty())
    return {CheckStatus::CheckFailed, std::nullopt};
  if (!IsNewerVersion(tag, CurrentVersion()))
    return {CheckStatus::UpToDate, std::nullopt};

  Update update;
  update.version = tag;
  update.title = ReadStringFromJson(obj, "name").value_or(tag);
  update.html_url = ReadStringFromJson(obj, "html_url").value_or(kReleasesPage);
  update.notes = ReadStringFromJson(obj, "body").value_or(std::string{});

  const auto assets = obj.find("assets");
  if (assets != obj.end() && assets->second.is<picojson::array>())
  {
    for (const picojson::value& a : assets->second.get<picojson::array>())
    {
      if (!a.is<picojson::object>())
        continue;
      const picojson::object& ao = a.get<picojson::object>();
      const std::string name = ReadStringFromJson(ao, "name").value_or(std::string{});
      if (name.size() < 9 || name.substr(name.size() - 9) != ".AppImage")
        continue;
      update.asset_url = ReadStringFromJson(ao, "browser_download_url").value_or(std::string{});
      const std::string digest = ReadStringFromJson(ao, "digest").value_or(std::string{});
      if (digest.rfind("sha256:", 0) == 0)
        update.asset_sha256 = digest.substr(7);
      break;
    }
  }
  return {CheckStatus::UpdateAvailable, update};
}

bool ShowUpdateDialog(const Update& update, QWidget* parent)
{
  QMessageBox box(parent);
  box.setIcon(QMessageBox::Information);
  box.setWindowTitle(QObject::tr("VibeDolphin Update"));
  box.setText(QObject::tr("VibeDolphin %1 is available.\n\nYou are running %2.")
                  .arg(QString::fromStdString(update.version))
                  .arg(QString::fromStdString(CurrentVersion())));
  if (!update.notes.empty())
    box.setDetailedText(QString::fromStdString(update.notes));

  const bool self_install = CanSelfInstall() && !update.asset_url.empty();
  QPushButton* const action = box.addButton(
      self_install ? QObject::tr("Update && Restart") : QObject::tr("Open release page"),
      QMessageBox::AcceptRole);
  box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
  box.setDefaultButton(action);
  box.exec();

  if (box.clickedButton() != action)
    return false;
  if (self_install)
    return SelfInstallAppImage(update, parent);  // true iff the app is now quitting to relaunch
  QDesktopServices::openUrl(QUrl(QString::fromStdString(update.html_url)));
  return false;
}
}  // namespace VibeUpdate

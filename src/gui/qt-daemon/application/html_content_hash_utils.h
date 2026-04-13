// Copyright (c) 2014-2024 Zano Project
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <string>
#include <vector>
#include <algorithm>

#include <QDirIterator>
#include <QFile>
#include <QCryptographicHash>

#include "misc_log_ex.h"

namespace gui_utils
{
  inline bool calculate_html_folder_hash(const std::string& html_path, std::string& result_hash)
  {
    TRY_ENTRY();

    QString base = QString::fromStdString(html_path).trimmed();
    QString root_dir;

    if (base.startsWith("qrc:/", Qt::CaseInsensitive))
    {
      root_dir = base.mid(3); // remove "qrc" prefix, keep ":/html"
    }
    else
    {
      QFileInfo fi(base);
      if (fi.isDir())
        root_dir = base;
      else
        root_dir = fi.dir().absolutePath();
    }

    QDirIterator it(root_dir, QDir::Files, QDirIterator::Subdirectories);
    std::vector<std::string> relative_paths;

    while (it.hasNext())
    {
      it.next();
      QString rel = it.filePath().mid(root_dir.length() + 1);
      rel.replace('\\', '/');
      std::string rel_str = rel.toStdString();
      if (rel_str.size() >= 4)
      {
        std::string ext = rel_str.substr(rel_str.rfind('.') + 1);
        if (ext == "map" || ext == "scss" || ext == "ts" || ext == "tsx")
          continue;
      }
      relative_paths.push_back(rel_str);
    }

    if (relative_paths.empty())
    {
      LOG_PRINT_RED("calculate_html_folder_hash: no files found in " << html_path, LOG_LEVEL_0);
      return false;
    }

    std::sort(relative_paths.begin(), relative_paths.end());

    std::string combined;
    for (const auto& rel : relative_paths)
    {
      QString full_path = root_dir + "/" + QString::fromStdString(rel);
      QFile file(full_path);
      if (!file.open(QIODevice::ReadOnly))
      {
        LOG_PRINT_RED("calculate_html_folder_hash: failed to open " << full_path.toStdString(), LOG_LEVEL_0);
        return false;
      }

      QCryptographicHash file_hasher(QCryptographicHash::Sha256);
      file_hasher.addData(&file);
      QString file_hash = file_hasher.result().toHex().toLower();
      file.close();

      combined += rel + ":" + file_hash.toStdString() + "\n";
    }

    // Final hash of the combined string
    QCryptographicHash final_hasher(QCryptographicHash::Sha256);
    final_hasher.addData(combined.c_str(), static_cast<int>(combined.size()));
    result_hash = final_hasher.result().toHex().toLower().toStdString();

    LOG_PRINT_L0("calculate_html_folder_hash: computed hash = " << result_hash << " (" << relative_paths.size() << " files)");
    return true;

    CATCH_ENTRY2(false);
  }
}

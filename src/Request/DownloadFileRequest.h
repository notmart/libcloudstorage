/*****************************************************************************
 * DownloadFileRequest.h : DownloadFileRequest headers
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef DOWNLOADFILEREQUEST_H
#define DOWNLOADFILEREQUEST_H

#include "Request.h"

namespace cloudstorage {

class DownloadStreamWrapper : public std::streambuf {
 public:
  DownloadStreamWrapper(std::function<void(const char*, uint32_t)> callback);
  std::streamsize xsputn(const char_type* data, std::streamsize length);

 private:
  std::function<void(const char*, uint32_t)> callback_;
};

class DownloadFileRequest : public Request {
 public:
  using Pointer = std::unique_ptr<DownloadFileRequest>;
  using RequestFactory =
      std::function<std::unique_ptr<HttpRequest>(const IItem&, std::ostream&)>;

  class ICallback {
   public:
    using Pointer = std::unique_ptr<ICallback>;
    virtual ~ICallback() = default;

    virtual void receivedData(const char* data, uint32_t length) = 0;
    virtual void done() = 0;
    virtual void error(const std::string& description) = 0;
    virtual void progress(uint32_t total, uint32_t now) = 0;
  };

  DownloadFileRequest(std::shared_ptr<CloudProvider>, IItem::Pointer file,
                      ICallback::Pointer, RequestFactory request_factory);
  ~DownloadFileRequest();

  void finish();

 private:
  int download(std::ostream& error_stream);

  std::future<void> function_;
  IItem::Pointer file_;
  DownloadStreamWrapper stream_wrapper_;
  ICallback::Pointer callback_;
  RequestFactory request_factory_;
};

}  // namespace cloudstorage

#endif  // DOWNLOADFILEREQUEST_H
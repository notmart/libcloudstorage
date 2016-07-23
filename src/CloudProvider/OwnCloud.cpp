/*****************************************************************************
 * OwnCloud.cpp : OwnCloud implementation
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "OwnCloud.h"

#include "Request/AuthorizeRequest.h"
#include "Utility/Item.h"

#include <curl/curl.h>
#include <cstring>

namespace cloudstorage {

OwnCloud::OwnCloud() : CloudProvider(make_unique<Auth>()) {}

IItem::Pointer OwnCloud::rootDirectory() const {
  return make_unique<Item>("root", "/", IItem::FileType::Directory);
}

void OwnCloud::initialize(const std::string& token,
                          ICloudProvider::ICallback::Pointer callback,
                          const ICloudProvider::Hints& hints) {
  CloudProvider::initialize(token, callback, hints);
  unpackCreditentials(token);
}

std::string OwnCloud::name() const { return "owncloud"; }

std::string OwnCloud::token() const {
  std::lock_guard<std::mutex> lock(auth_mutex());
  return user_ + "@" + owncloud_base_url_ + "##" + password_;
}

AuthorizeRequest::Pointer OwnCloud::authorizeAsync() {
  return make_unique<AuthorizeRequest>(
      shared_from_this(), [=](AuthorizeRequest* r) -> bool {
        if (callback()->userConsentRequired(*this) !=
            ICallback::Status::WaitForAuthorizationCode)
          return false;
        unpackCreditentials(r->getAuthorizationCode());
        return true;
      });
}

ICloudProvider::CreateDirectoryRequest::Pointer OwnCloud::createDirectoryAsync(
    IItem::Pointer parent, const std::string& name,
    CreateDirectoryCallback callback) {
  auto r = make_unique<Request<IItem::Pointer>>(shared_from_this());
  r->set_resolver([=](Request<IItem::Pointer>* r) -> IItem::Pointer {
    std::stringstream response;
    int code = r->sendRequest(
        [=](std::ostream&) {
          return make_unique<HttpRequest>(
              api_url() + "/remote.php/webdav" + parent->id() + name + "/",
              "MKCOL");
        },
        response);
    if (HttpRequest::isSuccess(code)) {
      IItem::Pointer item = make_unique<Item>(name, parent->id() + name + "/",
                                              IItem::FileType::Directory);
      callback(item);
      return item;
    } else {
      callback(nullptr);
      return nullptr;
    }
  });
  return r;
}

HttpRequest::Pointer OwnCloud::getItemDataRequest(const std::string& id,
                                                  std::ostream&) const {
  auto request = make_unique<HttpRequest>(api_url() + "/remote.php/webdav" + id,
                                          "PROPFIND");
  request->setHeaderParameter("Depth", "0");
  return request;
}

HttpRequest::Pointer OwnCloud::listDirectoryRequest(const IItem& item,
                                                    const std::string&,
                                                    std::ostream&) const {
  auto request = make_unique<HttpRequest>(
      api_url() + "/remote.php/webdav" + item.id(), "PROPFIND");
  request->setHeaderParameter("Depth", "1");
  return request;
}

HttpRequest::Pointer OwnCloud::uploadFileRequest(
    const IItem& directory, const std::string& filename, std::istream& stream,
    std::ostream& input_stream) const {
  auto request = make_unique<HttpRequest>(
      api_url() + "/remote.php/webdav" + directory.id() + filename,
      HttpRequest::Type::PUT);
  input_stream << stream.rdbuf();
  return request;
}

HttpRequest::Pointer OwnCloud::downloadFileRequest(const IItem& item,
                                                   std::ostream&) const {
  return make_unique<HttpRequest>(static_cast<const Item&>(item).url(),
                                  HttpRequest::Type::GET);
}

HttpRequest::Pointer OwnCloud::deleteItemRequest(
    const IItem& item, std::ostream& input_stream) const {
  auto request = make_unique<HttpRequest>(
      api_url() + "/remote.php/webdav" + item.id(), "DELETE");
  return request;
}

HttpRequest::Pointer OwnCloud::moveItemRequest(const IItem& source,
                                               const IItem& destination,
                                               std::ostream&) const {
  auto request = make_unique<HttpRequest>(
      api_url() + "/remote.php/webdav" + source.id(), "MOVE");
  request->setHeaderParameter(
      "Destination", "https://" + owncloud_base_url_ + "/remote.php/webdav" +
                         destination.id() + "/" + source.filename());
  return request;
}

IItem::Pointer OwnCloud::getItemDataResponse(std::istream& stream) const {
  std::stringstream sstream;
  sstream << stream.rdbuf();
  tinyxml2::XMLDocument document;
  if (document.Parse(sstream.str().c_str(), sstream.str().size()) !=
      tinyxml2::XML_NO_ERROR)
    return nullptr;
  return toItem(document.RootElement()->FirstChild());
}

std::vector<IItem::Pointer> OwnCloud::listDirectoryResponse(
    std::istream& stream, std::string&) const {
  std::stringstream sstream;
  sstream << stream.rdbuf();
  tinyxml2::XMLDocument document;
  if (document.Parse(sstream.str().c_str(), sstream.str().size()) !=
      tinyxml2::XML_NO_ERROR)
    return {};
  if (document.RootElement()->FirstChild() == nullptr) return {};

  std::vector<IItem::Pointer> result;
  for (auto child = document.RootElement()->FirstChild()->NextSibling(); child;
       child = child->NextSibling()) {
    result.push_back(toItem(child));
  }
  return result;
}

std::string OwnCloud::api_url() const {
  std::lock_guard<std::mutex> lock(auth_mutex());
  return "https://" + user_ + ":" + password_ + "@" + owncloud_base_url_;
}

IItem::Pointer OwnCloud::toItem(const tinyxml2::XMLNode* node) const {
  std::string id = node->FirstChildElement("d:href")->GetText();
  id = id.substr(strlen("/remote.php/webdav"));
  IItem::FileType type = IItem::FileType::Unknown;
  if (id.back() == '/') type = IItem::FileType::Directory;
  std::string filename = id;
  if (filename.back() == '/') filename.pop_back();
  filename = filename.substr(filename.find_last_of('/') + 1);
  auto item = make_unique<Item>(unescape(filename), id, type);
  item->set_url(api_url() + "/remote.php/webdav" + id);
  return item;
}

bool OwnCloud::reauthorize(int code) const {
  return CloudProvider::reauthorize(code) || HttpRequest::isCurlError(code);
}

void OwnCloud::authorizeRequest(HttpRequest&) const {}

std::string OwnCloud::unescape(const std::string& str) const {
  auto handle = curl_easy_init();
  int length = 0;
  char* data = curl_easy_unescape(handle, str.c_str(), str.length(), &length);
  std::string result(data, length);
  free(data);
  curl_easy_cleanup(handle);
  return result;
}

void OwnCloud::unpackCreditentials(const std::string& code) {
  std::unique_lock<std::mutex> lock(auth_mutex());
  auto separator = code.find_first_of("##");
  auto at_position = code.find_last_of('@', separator);
  if (at_position == std::string::npos || separator == std::string::npos)
    return;
  user_ = code.substr(0, at_position);
  owncloud_base_url_ =
      code.substr(at_position + 1, separator - at_position - 1);
  password_ = code.substr(separator + strlen("##"));
}

OwnCloud::Auth::Auth() {}

std::string OwnCloud::Auth::authorizeLibraryUrl() const {
  return redirect_uri() + "/login";
}

HttpRequest::Pointer OwnCloud::Auth::exchangeAuthorizationCodeRequest(
    std::ostream&) const {
  return nullptr;
}

HttpRequest::Pointer OwnCloud::Auth::refreshTokenRequest(std::ostream&) const {
  return nullptr;
}

IAuth::Token::Pointer OwnCloud::Auth::exchangeAuthorizationCodeResponse(
    std::istream&) const {
  return nullptr;
}

IAuth::Token::Pointer OwnCloud::Auth::refreshTokenResponse(
    std::istream&) const {
  return nullptr;
}

}  // namespace cloudstorage

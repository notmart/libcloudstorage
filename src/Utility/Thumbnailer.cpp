/*****************************************************************************
 * FFmpegThumbnailer.cpp : FFmpegThumbnailer implementation
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

#include "Thumbnailer.h"

#include "CloudProvider/CloudProvider.h"

namespace cloudstorage {

Thumbnailer::Thumbnailer(IThumbnailer::Pointer t)
    : destroyed_(false), thumbnailer_(std::move(t)) {
  cleanup_thread_ = std::async(std::launch::async, [this]() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      condition_.wait(lock,
                      [this]() { return destroyed_ || finished_.size() > 0; });
      while (finished_.size() > 0) {
        auto it = thumbnail_threads_.find(finished_.back());
        if (it != thumbnail_threads_.end()) {
          it->second.future_.wait();
          thumbnail_threads_.erase(it);
        }
        finished_.pop_back();
      }
      if (destroyed_) break;
    }
  });
}

Thumbnailer::~Thumbnailer() {
  for (const auto& t : thumbnail_threads_) t.second.future_.wait();
  destroyed_ = true;
  condition_.notify_one();
  cleanup_thread_.wait();
}

IRequest<EitherError<std::vector<char>>>::Pointer
Thumbnailer::generateThumbnail(std::shared_ptr<ICloudProvider> p,
                               IItem::Pointer item) {
  auto r = util::make_unique<ThumbnailRequest>(
      std::static_pointer_cast<CloudProvider>(p));
  r->set_resolver([this, item](Request<EitherError<std::vector<char>>>* r)
                      -> EitherError<std::vector<char>> {
    if ((item->type() != IItem::FileType::Image &&
         item->type() != IItem::FileType::Video) ||
        item->url().empty()) {
      Error e{IHttpRequest::Failure,
              "can generate thumbnails only for images and videos"};
      return e;
    }
    std::shared_future<EitherError<std::vector<char>>> future;
    std::shared_ptr<std::condition_variable> done;
    std::shared_ptr<bool> finished;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = thumbnail_threads_.find(item->url());
      std::string url = item->url();
      if (it == std::end(thumbnail_threads_)) {
        done = std::make_shared<std::condition_variable>();
        finished = std::make_shared<bool>(false);
        future = std::async(std::launch::async, [this, item, done, finished]() {
          auto result = thumbnailer_->generateThumbnail(item);
          {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_.push_back(item->url());
            *finished = true;
          }
          done->notify_all();
          condition_.notify_one();
          return result;
        });
        thumbnail_threads_[url] = {future, done, finished};
      } else {
        future = it->second.future_;
        done = it->second.done_;
        finished = it->second.finished_;
      }
    }
    static_cast<ThumbnailRequest*>(r)->set_cancel_callback(
        [done]() { done->notify_all(); });
    {
      std::unique_lock<std::mutex> lock(mutex_);
      done->wait(lock, [=]() { return r->is_cancelled() || *finished; });
    }
    if (r->is_cancelled()) return Error{IHttpRequest::Aborted, ""};
    return future.get();
  });
  return std::move(r);
}

void Thumbnailer::ThumbnailRequest::cancel() {
  if (is_cancelled()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_callback_) cancel_callback_();
  }
  Request::cancel();
}

void Thumbnailer::ThumbnailRequest::set_cancel_callback(
    std::function<void()> f) {
  std::lock_guard<std::mutex> lock(mutex_);
  cancel_callback_ = f;
}

}  // namespace cloudstorage
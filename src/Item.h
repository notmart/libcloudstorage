/*****************************************************************************
 * Item.h : interface for Item
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

#ifndef ITEM_H
#define ITEM_H

#include <string>
#include "IItem.h"

namespace cloudstorage {

class Item : public IItem {
 public:
  Item(std::string filename, std::string id, bool is_directory);

  std::string filename() const;
  std::string id() const;
  bool is_directory() const;

 private:
  std::string filename_;
  std::string id_;
  bool is_directory_;
};

}  // namespace cloudstorage

#endif  // ITEM_H
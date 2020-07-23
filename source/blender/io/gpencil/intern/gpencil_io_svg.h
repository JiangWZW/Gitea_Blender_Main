/*
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#pragma once

/** \file
 * \ingroup bgpencil
 */
#include "pugixml.hpp"

struct Main;
struct GpencilExportParams;

namespace blender {
namespace io {
namespace gpencil {

class Gpencilwriter {

 public:
  Gpencilwriter(const struct GpencilExportParams *params);
  bool export_object(void);

 private:
  GpencilExportParams params;
};

class GpencilwriterSVG {

 public:
  GpencilwriterSVG(struct GpencilExportParams *params);
  bool write(void);

 private:
  GpencilExportParams *params;
  pugi::xml_document doc;
};

}  // namespace gpencil
}  // namespace io
}  // namespace blender

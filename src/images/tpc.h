/* Phaethon - A FLOSS resource explorer for BioWare's Aurora engine games
 *
 * Phaethon is the legal property of its developers, whose names
 * can be found in the AUTHORS file distributed with this source
 * distribution.
 *
 * Phaethon is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * Phaethon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Phaethon. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file
 *  TPC (BioWare's own texture format) loading.
 */

#ifndef IMAGES_TPC_H
#define IMAGES_TPC_H

#include <memory>

#include "src/images/decoder.h"

namespace Common {
	class SeekableReadStream;
}

namespace Images {

/** BioWare's own texture format, TPC. */
class TPC : public Decoder {
public:
	TPC(Common::SeekableReadStream &tpc);
	~TPC();

	/** Return the enclosed TXI data. */
	Common::SeekableReadStream *getTXI() const;

private:
	std::unique_ptr<byte[]> _txiData;
	size_t _txiDataSize;

	// Loading helpers
	void load(Common::SeekableReadStream &tpc);
	void readHeader(Common::SeekableReadStream &tpc, byte &encoding);
	void readData(Common::SeekableReadStream &tpc, byte encoding);
	void readTXIData(Common::SeekableReadStream &tpc);

	bool checkCubeMap(uint32_t &width, uint32_t &height);
	void fixupCubeMap();

	static void deSwizzle(byte *dst, const byte *src, uint32_t width, uint32_t height);
};

} // End of namespace Images

#endif // IMAGES_TPC_H

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
 *  Decoding Microsoft's Windows Media Audio.
 */

#ifndef SOUND_DECODERS_WMA_H
#define SOUND_DECODERS_WMA_H

#include "src/common/types.h"

namespace Common {
class SeekableReadStream;
}

namespace Sound {

class PacketizedAudioStream;

/**
 * Create a PacketizedAudioStream that decodes WMA sound
 *
 * @param extraData  The stream containing the extra data needed for initialization
 * @return             A new PacketizedAudioStream, or NULL on error
 */
PacketizedAudioStream *makeWMAStream(int version, uint32 sampleRate, uint8 channels,
	uint32 bitRate, uint32 blockAlign, Common::SeekableReadStream &extraData);

} // End of namespace Sound

#endif // SOUND_DECODERS_WMA_H

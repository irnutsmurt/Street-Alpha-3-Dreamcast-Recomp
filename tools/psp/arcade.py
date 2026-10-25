"""Arcade mode's routes for the PSP characters.

The Dreamcast keeps 16 routes per character (8C0A8248: a pointer per
character to 16 x 8 opponents; the game picks one by its route number, game
state +0x21 & 15) and adds the 9th and 10th opponents from tables (the rival
8C0A7176, the boss 8C0A719E). f_8C02BD08 builds the route; a pattern (game
state +0x22) then swaps in the other Chun-Li (0x12) and Sodom (0x14).

The PSP makes the route when the game starts instead (f_2ACD0): a Mersenne
twister seeded with the route number + 0x1C3, then for each of the 10 places:
  4      the character's 5th opponent (0x20A7D8)
  8, 9   the rival and the boss (as on the Dreamcast)
  others each of the character's weighted opponents (0x20ACC0: records of
         opponent and 10 chances out of 256, one per place) gets a random
         byte; the first under its chance that fits is taken (f_2AF74).
         Failing that, random numbers % 42 until one fits that isn't in the
         character's exclusion list (0x20AA24) (f_2AEE8).
  fits:  not already in the route (the other Chun-Li or Sodom counting as
         the same) and not from the previous opponent's stage (0x20B604)
         (f_2B024).
Running that for each route number gives the PSP's 16 routes, which go in
the Dreamcast's form.
"""
import struct

ROUTES = 0x8C0A8248
VARIANTS, PLACES = 16, 8

PSP_OFF = 0x74
PSP_FIFTH, PSP_RIVAL, PSP_BOSS = 0x20A7D8, 0x20A784, 0x20A7AE
PSP_WEIGHTED, PSP_EXCLUDED, PSP_STAGES = 0x20ACC0, 0x20AA24, 0x20B604
SEED = 0x1C3


class Twister:
    """MT19937 as the PSP's f_1CBE0 uses it: the low byte of each tempered word"""
    def __init__(self, seed):
        self.mt = [seed & 0xFFFFFFFF]
        for i in range(1, 624):
            p = self.mt[-1]
            self.mt.append((0x6C078965 * (p ^ (p >> 30)) + i) & 0xFFFFFFFF)
        self.i = 0

    def twist(self):
        mt = self.mt
        for k in range(624):
            y = (mt[k] & 0x80000000) | (mt[(k + 1) % 624] & 0x7FFFFFFF)
            mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)

    def byte(self):
        if self.i == 0:
            self.twist()
        y = self.mt[self.i]
        self.i = (self.i + 1) % 624
        y ^= y >> 11
        y ^= (y << 7) & 0x9D2C5680
        y ^= (y << 15) & 0xEFC60000
        y ^= y >> 18
        return y & 0xFF


class Psp:
    def __init__(self, boot):
        self.boot = boot

    def u8(self, a):
        return self.boot[a + PSP_OFF]

    def s8(self, a):
        v = self.u8(a)
        return v - 256 if v & 0x80 else v

    def u32(self, a):
        return struct.unpack_from("<I", self.boot, a + PSP_OFF)[0]

    def list(self, table, c, size):
        """a per-character list of size-byte records, 0xFF first byte ends it"""
        a, out = self.u32(table + 4 * c), []
        while self.u8(a) != 0xFF:
            out.append(bytes(self.boot[a + PSP_OFF:a + PSP_OFF + size]))
            a += size
        return out

    def route(self, c, variant):
        rng = Twister(variant + SEED)
        weighted = self.list(PSP_WEIGHTED, c, 11)
        excluded = [r[0] for r in self.list(PSP_EXCLUDED, c, 1)]
        route = []

        def fits(o, place, never=()):
            for x in route:
                if x == o or (o == 4 and x == 0x12) or (o == 6 and x == 0x14):
                    return False
            if place > 0 and self.s8(PSP_STAGES + route[-1]) == self.s8(PSP_STAGES + o):
                return False
            return o not in never

        for place in range(10):
            if place == 4:
                o = self.u8(PSP_FIFTH + c)
            elif place == 8:
                o = self.u8(PSP_RIVAL + c)
            elif place == 9:
                o = self.u8(PSP_BOSS + c)
            else:
                o = None
                for rec in weighted:
                    if rng.byte() < rec[1 + place] and fits(rec[0], place):
                        o = rec[0]
                        break
                while o is None:
                    k = rng.byte() % 42
                    if fits(k, place, excluded):
                        o = k
            route.append(o)
        return route


def patches(boot, chars, data, routes_ext):
    """[(address, bytes)]: each character's 16 routes at data and on, its
    entry in the moved route table (at routes_ext); the end of the data"""
    psp, out = Psp(boot), []
    for c in chars:
        lists = b"".join(bytes(psp.route(c, v)[:PLACES]) for v in range(VARIANTS))
        out.append((data, lists))
        out.append((routes_ext + 4 * c, struct.pack("<I", data)))
        data += len(lists)
    return out, data


if __name__ == "__main__":
    import sys
    p = Psp(open(sys.argv[1], "rb").read())
    for c in [int(a, 16) for a in sys.argv[2:]] or (0x26, 0x27, 0x28, 0x29):
        print(f"{c:02X}:")
        for v in range(VARIANTS):
            print("   ", " ".join(f"{o:02X}" for o in p.route(c, v)))

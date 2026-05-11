import struct
import zmq
from datetime import datetime, timezone

# struct Header {
#     int64_t seconds;
#     int64_t femtoseconds;
#     double rssi;
#     uint32_t msg_len;
#     uint32_t ncoh;
#     uint32_t nsymbols;
#     uint32_t vtype;
# };
Header = struct.Struct("<qqdIIII")

if __name__ == "__main__":
    context = zmq.Context()
    socket = context.socket(zmq.SUB)
    socket.connect("tcp://localhost:28650")
    socket.setsockopt_string(zmq.SUBSCRIBE, "")

    while True:
        try:
            f_hdr, f_msg, _ = socket.recv_multipart(copy=False)
            (
                seconds,
                femtoseconds,
                rssi,
                msg_len,
                *_,
            ) = Header.unpack(f_hdr.buffer)
            timestamp = datetime.fromtimestamp(seconds + femtoseconds * 1e-15, tz=timezone.utc)
            message = bytes(f_msg.buffer)[:msg_len].hex().upper()
            print(f"{timestamp} | RSSI: {rssi} dB | Message: {message}")
        except KeyboardInterrupt:
            break
        except zmq.error.Again:
            continue
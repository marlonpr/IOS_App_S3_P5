#!/usr/bin/env python3
"""Hardware test tool for the ESP32 Color Palette TCP protocol.

Run this only after flashing firmware that supports LP/CP/DP.

Examples:
    python tools/palette_tcp_test.py --host 192.168.10.51 --board-id 0
    python tools/palette_tcp_test.py --host 192.168.10.51 --no-write
    python tools/palette_tcp_test.py --host 192.168.10.51 --test-delimiter-color
    python tools/palette_tcp_test.py --host 192.168.10.51 read --mode 1
    python tools/palette_tcp_test.py --host 192.168.10.51 defaults --mode 1
    python tools/palette_tcp_test.py --host 192.168.10.51 full-test
"""

from __future__ import annotations

import argparse
import socket
import sys
from dataclasses import dataclass
from typing import Mapping


TERMINATOR = 0x5C
PALETTE_SCHEMA_VERSION = 0x01
MAX_FRAME_SIZE = 256
DEFAULT_PORT = 5000
DEFAULT_TIMEOUT_SECONDS = 5.0
TEST_DATE_ROLE = 0x02
DEFAULT_TEST_COLOR = "FF00FF"
DELIMITER_TEST_COLOR = "005CFF"

STATUS_NAMES = {
    0x00: "success",
    0x01: "unsupported mode",
    0x02: "unsupported version",
    0x03: "invalid count or invalid length",
    0x04: "duplicate role",
    0x05: "unsupported role",
    0x06: "incomplete required role set",
    0x07: "invalid hex",
    0x08: "NVS failure",
    0x09: "busy",
    0x0A: "internal failure",
}

ROLE_NAMES = {
    0x01: "time",
    0x02: "date",
    0x03: "weekday",
    0x10: "temperature_cold",
    0x11: "temperature_cool",
    0x12: "temperature_warm",
    0x13: "temperature_hot",
}

FACTORY_PALETTES: dict[int, dict[int, str]] = {
    1: {
        0x01: "FFFFFF",
        0x02: "00FF00",
        0x10: "FFFFFF",
        0x11: "00FFFF",
        0x12: "FF4100",
        0x13: "FF0000",
    },
    2: {
        0x01: "FFFFFF",
        0x02: "0000FF",
        0x10: "FFFFFF",
        0x11: "00FFFF",
        0x12: "FF4100",
        0x13: "FF0000",
    },
    3: {
        0x01: "FFFFFF",
        0x02: "0000FF",
        0x03: "00FF00",
        0x10: "FFFFFF",
        0x11: "00FFFF",
        0x12: "FF4100",
        0x13: "FF0000",
    },
}


class PaletteTestError(RuntimeError):
    """Raised when a hardware exchange or protocol validation fails."""


@dataclass(frozen=True)
class LPResponse:
    mode: int
    status: int
    version: int
    entries: dict[int, str]


@dataclass(frozen=True)
class AckResponse:
    command: str
    mode: int
    status: int


def parse_byte_argument(value: str) -> int:
    try:
        parsed = int(value, 16) if value.lower().startswith("0x") else int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"expected a byte value, got {value!r}"
        ) from exc

    if not 0 <= parsed <= 0xFF:
        raise argparse.ArgumentTypeError("value must be between 0 and 255")
    return parsed


def format_status(status: int) -> str:
    return f"{status:02X} ({STATUS_NAMES.get(status, 'unknown status')})"


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in data)


def friendly_ascii(data: bytes) -> str:
    return "".join(chr(byte) if 32 <= byte <= 126 else "." for byte in data)


def print_frame(direction: str, frame: bytes) -> None:
    print(f"{direction} {len(frame)} bytes")
    print(f"  HEX:   {hex_bytes(frame)}")
    print(f"  ASCII: {friendly_ascii(frame)}")


def encode_hex_byte(value: int) -> bytes:
    if not 0 <= value <= 0xFF:
        raise PaletteTestError(f"hex byte out of range: {value}")
    return f"{value:02X}".encode("ascii")


def decode_hex_byte(data: bytes, field_name: str) -> int:
    if len(data) != 2:
        raise PaletteTestError(f"{field_name} must contain exactly two hex characters")

    try:
        text = data.decode("ascii")
        if any(character not in "0123456789ABCDEFabcdef" for character in text):
            raise ValueError
        return int(text, 16)
    except (UnicodeDecodeError, ValueError) as exc:
        raise PaletteTestError(
            f"{field_name} contains invalid ASCII hexadecimal: {data!r}"
        ) from exc


def normalize_rgb(value: str, field_name: str = "RGB") -> str:
    normalized = value.upper()
    if len(normalized) != 6 or any(
        character not in "0123456789ABCDEF" for character in normalized
    ):
        raise PaletteTestError(
            f"{field_name} must be exactly six ASCII hexadecimal characters"
        )
    return normalized


def decode_rgb(data: bytes, field_name: str) -> str:
    try:
        return normalize_rgb(data.decode("ascii"), field_name)
    except UnicodeDecodeError as exc:
        raise PaletteTestError(f"{field_name} is not ASCII") from exc


def build_prefix(board_id: int, command: bytes) -> bytes:
    if len(command) != 2:
        raise PaletteTestError("protocol command must be exactly two bytes")
    if board_id == TERMINATOR:
        raise PaletteTestError("board ID 0x5C conflicts with the frame terminator")
    return b"/TA" + bytes((board_id,)) + command


def build_lp_request(board_id: int, mode: int) -> bytes:
    return build_prefix(board_id, b"LP") + encode_hex_byte(mode) + b"\\"


def build_dp_request(board_id: int, mode: int) -> bytes:
    return build_prefix(board_id, b"DP") + encode_hex_byte(mode) + b"\\"


def build_cp_request(
    board_id: int,
    mode: int,
    palette: Mapping[int, str],
) -> bytes:
    if mode not in FACTORY_PALETTES:
        raise PaletteTestError(f"mode {mode} does not have an editable palette")

    expected_roles = set(FACTORY_PALETTES[mode])
    supplied_roles = set(palette)
    if supplied_roles != expected_roles:
        missing = sorted(expected_roles - supplied_roles)
        extra = sorted(supplied_roles - expected_roles)
        raise PaletteTestError(
            f"CP Mode {mode} role set mismatch: missing={missing}, extra={extra}"
        )

    entries = bytearray()
    for role in sorted(palette):
        entries.extend(encode_hex_byte(role))
        entries.extend(normalize_rgb(palette[role], f"role {role:02X} RGB").encode("ascii"))

    return (
        build_prefix(board_id, b"CP")
        + encode_hex_byte(mode)
        + encode_hex_byte(PALETTE_SCHEMA_VERSION)
        + encode_hex_byte(len(palette))
        + bytes(entries)
        + b"\\"
    )


def validate_response_prefix(
    frame: bytes,
    board_id: int,
    expected_command: bytes,
) -> None:
    if len(frame) < 7:
        raise PaletteTestError(f"response is too short: {len(frame)} bytes")
    if frame[:3] != b"/ta":
        raise PaletteTestError(f"invalid response prefix: {frame[:3]!r}")
    if frame[3] != board_id:
        raise PaletteTestError(
            f"response board ID mismatch: got 0x{frame[3]:02X}, "
            f"expected 0x{board_id:02X}"
        )
    if frame[4:6] != expected_command:
        raise PaletteTestError(
            f"response command mismatch: got {frame[4:6]!r}, "
            f"expected {expected_command!r}"
        )
    if frame[-1] != TERMINATOR:
        raise PaletteTestError("response is missing the 0x5C terminator")


def validate_known_status(status: int) -> None:
    if status not in STATUS_NAMES:
        raise PaletteTestError(f"firmware returned unknown status 0x{status:02X}")


def parse_lp_response(frame: bytes, board_id: int) -> LPResponse:
    validate_response_prefix(frame, board_id, b"lp")
    if len(frame) < 15:
        raise PaletteTestError(f"LP response is too short: {len(frame)} bytes")

    mode = decode_hex_byte(frame[6:8], "LP mode")
    status = decode_hex_byte(frame[8:10], "LP status")
    version = decode_hex_byte(frame[10:12], "LP version")
    count = decode_hex_byte(frame[12:14], "LP role count")
    validate_known_status(status)

    expected_length = 15 + (count * 8)
    if len(frame) != expected_length:
        raise PaletteTestError(
            f"LP response length mismatch: got {len(frame)}, "
            f"expected {expected_length} for count {count}"
        )

    entries: dict[int, str] = {}
    offset = 14
    for index in range(count):
        role = decode_hex_byte(frame[offset : offset + 2], f"LP role {index}")
        color = decode_rgb(frame[offset + 2 : offset + 8], f"LP role {role:02X} RGB")
        if role in entries:
            raise PaletteTestError(f"LP response contains duplicate role {role:02X}")
        entries[role] = color
        offset += 8

    if status == 0x00:
        if version != PALETTE_SCHEMA_VERSION:
            raise PaletteTestError(
                f"LP success returned schema {version:02X}, expected 01"
            )
        if count == 0:
            raise PaletteTestError("LP success returned an empty palette")
    elif version != 0 or count != 0:
        raise PaletteTestError(
            "LP error response must contain version 00, count 00, and no entries"
        )

    return LPResponse(mode=mode, status=status, version=version, entries=entries)


def parse_ack_response(
    frame: bytes,
    board_id: int,
    expected_command: bytes,
) -> AckResponse:
    validate_response_prefix(frame, board_id, expected_command)
    if len(frame) != 11:
        raise PaletteTestError(
            f"{expected_command.decode().upper()} ACK length mismatch: "
            f"got {len(frame)}, expected 11"
        )

    mode = decode_hex_byte(frame[6:8], "ACK mode")
    status = decode_hex_byte(frame[8:10], "ACK status")
    validate_known_status(status)
    return AckResponse(
        command=expected_command.decode("ascii"),
        mode=mode,
        status=status,
    )


class PaletteTCPClient:
    def __init__(self, host: str, port: int, board_id: int, timeout: float) -> None:
        self.host = host
        self.port = port
        self.board_id = board_id
        self.timeout = timeout
        self._socket: socket.socket | None = None
        self._receive_buffer = bytearray()

    def __enter__(self) -> "PaletteTCPClient":
        print(f"Connecting to {self.host}:{self.port} (timeout {self.timeout:g}s)...")
        try:
            self._socket = socket.create_connection(
                (self.host, self.port), timeout=self.timeout
            )
            self._socket.settimeout(self.timeout)
        except OSError as exc:
            raise PaletteTestError(
                f"could not connect to {self.host}:{self.port}: {exc}"
            ) from exc
        print("Connected.")
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def close(self) -> None:
        if self._socket is None:
            return
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._socket.close()
        self._socket = None
        print("Connection closed.")

    def _recv(self, description: str) -> bytes:
        if self._socket is None:
            raise PaletteTestError("TCP socket is not connected")
        try:
            chunk = self._socket.recv(MAX_FRAME_SIZE)
        except socket.timeout as exc:
            raise PaletteTestError(
                f"timed out after {self.timeout:g}s waiting for {description}"
            ) from exc
        except OSError as exc:
            raise PaletteTestError(
                f"TCP receive failed while waiting for {description}: {exc}"
            ) from exc

        if not chunk:
            raise PaletteTestError(
                f"ESP32 closed the connection while waiting for {description}"
            )
        return chunk

    def read_palette_frame(self, description: str) -> bytes:
        while True:
            try:
                terminator_index = self._receive_buffer.index(TERMINATOR)
            except ValueError:
                terminator_index = -1

            if terminator_index >= 0:
                if terminator_index + 1 > MAX_FRAME_SIZE:
                    raise PaletteTestError(
                        f"{description} exceeded the "
                        f"{MAX_FRAME_SIZE}-byte response limit"
                    )
                frame = bytes(self._receive_buffer[: terminator_index + 1])
                del self._receive_buffer[: terminator_index + 1]
                return frame

            if len(self._receive_buffer) >= MAX_FRAME_SIZE:
                raise PaletteTestError(
                    f"{description} exceeded the {MAX_FRAME_SIZE}-byte response limit"
                )
            self._receive_buffer.extend(self._recv(description))
            if len(self._receive_buffer) > MAX_FRAME_SIZE:
                try:
                    terminator_index = self._receive_buffer.index(TERMINATOR)
                except ValueError:
                    terminator_index = -1
                if terminator_index < 0 or terminator_index + 1 > MAX_FRAME_SIZE:
                    raise PaletteTestError(
                        f"{description} exceeded the "
                        f"{MAX_FRAME_SIZE}-byte response limit"
                    )

    def read_exact(self, length: int, description: str) -> bytes:
        while len(self._receive_buffer) < length:
            self._receive_buffer.extend(self._recv(description))
            if len(self._receive_buffer) > MAX_FRAME_SIZE:
                raise PaletteTestError(
                    f"{description} exceeded the {MAX_FRAME_SIZE}-byte response limit"
                )

        response = bytes(self._receive_buffer[:length])
        del self._receive_buffer[:length]
        return response

    def send(self, frame: bytes, description: str) -> None:
        if self._socket is None:
            raise PaletteTestError("TCP socket is not connected")
        if len(frame) > MAX_FRAME_SIZE:
            raise PaletteTestError(
                f"{description} exceeds the {MAX_FRAME_SIZE}-byte frame limit"
            )
        print_frame(f"TX {description}", frame)
        try:
            self._socket.sendall(frame)
        except socket.timeout as exc:
            raise PaletteTestError(
                f"timed out after {self.timeout:g}s sending {description}"
            ) from exc
        except OSError as exc:
            raise PaletteTestError(f"TCP send failed for {description}: {exc}") from exc

    def exchange_palette(self, frame: bytes, description: str) -> bytes:
        self.send(frame, description)
        response = self.read_palette_frame(f"{description} response")
        print_frame(f"RX {description}", response)
        return response


def require_status(actual: int, expected: int, operation: str) -> None:
    if actual != expected:
        raise PaletteTestError(
            f"{operation} returned {format_status(actual)}, "
            f"expected {format_status(expected)}"
        )


def validate_complete_palette(response: LPResponse) -> None:
    expected = FACTORY_PALETTES.get(response.mode)
    if expected is None:
        raise PaletteTestError(f"Mode {response.mode} has no editable palette")

    actual_roles = set(response.entries)
    expected_roles = set(expected)
    if actual_roles != expected_roles:
        missing = sorted(expected_roles - actual_roles)
        extra = sorted(actual_roles - expected_roles)
        raise PaletteTestError(
            f"LP Mode {response.mode} role set mismatch: "
            f"missing={missing}, extra={extra}"
        )


def print_palette(response: LPResponse) -> None:
    print(
        f"  LP parsed: mode={response.mode:02X} "
        f"status={format_status(response.status)} "
        f"version={response.version:02X} count={len(response.entries):02X}"
    )
    for role, color in sorted(response.entries.items()):
        role_name = ROLE_NAMES.get(role, f"unknown_{role:02X}")
        print(f"    {role:02X} {role_name:<20} #{color}")


def print_factory_comparison(response: LPResponse) -> None:
    factory = FACTORY_PALETTES[response.mode]
    differences = [
        (role, factory[role], response.entries[role])
        for role in sorted(factory)
        if response.entries[role] != factory[role]
    ]
    if not differences:
        print(f"  Mode {response.mode} matches compiled factory defaults.")
        return

    print(f"  Mode {response.mode} has active overrides/differences:")
    for role, expected, actual in differences:
        print(
            f"    {role:02X} {ROLE_NAMES[role]}: "
            f"active #{actual}, factory #{expected}"
        )


def read_palette(
    client: PaletteTCPClient,
    mode: int,
    expected_status: int = 0x00,
) -> LPResponse:
    request = build_lp_request(client.board_id, mode)
    frame = client.exchange_palette(request, f"LP Mode {mode}")
    response = parse_lp_response(frame, client.board_id)
    print_palette(response)

    if response.mode != mode:
        raise PaletteTestError(
            f"LP response mode mismatch: got {response.mode}, expected {mode}"
        )
    require_status(response.status, expected_status, f"LP Mode {mode}")
    if response.status == 0x00:
        validate_complete_palette(response)
    return response


def save_palette(
    client: PaletteTCPClient,
    mode: int,
    palette: Mapping[int, str],
    verify_delimiter_safety: bool,
) -> AckResponse:
    request = build_cp_request(client.board_id, mode, palette)

    if verify_delimiter_safety:
        if request[:-1].find(bytes((TERMINATOR,))) >= 0:
            raise PaletteTestError(
                "CP palette payload contains a raw 0x5C before the terminator"
            )
        if DELIMITER_TEST_COLOR.encode("ascii") not in request:
            raise PaletteTestError("CP request does not contain ASCII 005CFF")
        print(
            "Delimiter safety confirmed: 005CFF is encoded as "
            "30 30 35 43 46 46; only the final byte is raw 0x5C."
        )

    frame = client.exchange_palette(request, f"CP Mode {mode}")
    response = parse_ack_response(frame, client.board_id, b"cp")
    print(
        f"  CP parsed: mode={response.mode:02X} "
        f"status={format_status(response.status)}"
    )
    if response.mode != mode:
        raise PaletteTestError(
            f"CP ACK mode mismatch: got {response.mode}, expected {mode}"
        )
    require_status(response.status, 0x00, f"CP Mode {mode}")
    return response


def restore_defaults(client: PaletteTCPClient, mode: int) -> AckResponse:
    frame = client.exchange_palette(
        build_dp_request(client.board_id, mode), f"DP Mode {mode}"
    )
    response = parse_ack_response(frame, client.board_id, b"dp")
    print(
        f"  DP parsed: mode={response.mode:02X} "
        f"status={format_status(response.status)}"
    )
    if response.mode != mode:
        raise PaletteTestError(
            f"DP ACK mode mismatch: got {response.mode}, expected {mode}"
        )
    require_status(response.status, 0x00, f"DP Mode {mode}")
    return response


def verify_factory_palette(response: LPResponse) -> None:
    expected = FACTORY_PALETTES[response.mode]
    if response.entries != expected:
        differences = [
            f"{role:02X}: got {response.entries.get(role)}, expected {color}"
            for role, color in expected.items()
            if response.entries.get(role) != color
        ]
        raise PaletteTestError(
            f"Mode {response.mode} did not return to factory defaults: "
            + "; ".join(differences)
        )


def attempt_cleanup(
    client: PaletteTCPClient,
    host: str,
    port: int,
    board_id: int,
    timeout: float,
    mode: int,
) -> None:
    print(f"Cleanup: attempting DP Mode {mode}...")
    try:
        restore_defaults(client, mode)
        print("Cleanup succeeded on the existing connection.")
        return
    except (PaletteTestError, OSError) as first_error:
        print(f"WARNING: cleanup on existing connection failed: {first_error}")

    try:
        with PaletteTCPClient(host, port, board_id, timeout) as cleanup_client:
            restore_defaults(cleanup_client, mode)
        print("Cleanup succeeded after reconnecting.")
    except (PaletteTestError, OSError) as reconnect_error:
        print(
            f"WARNING: cleanup failed after reconnecting: {reconnect_error}. "
            f"Mode {mode} may still contain the temporary test palette."
        )


def run_temporary_save_test(
    client: PaletteTCPClient,
    host: str,
    port: int,
    timeout: float,
    mode: int,
    test_delimiter_color: bool,
) -> None:
    initial = read_palette(client, mode)
    print_factory_comparison(initial)

    test_color = DELIMITER_TEST_COLOR if test_delimiter_color else DEFAULT_TEST_COLOR
    test_palette = dict(initial.entries)
    test_palette[TEST_DATE_ROLE] = test_color
    cleanup_required = True

    try:
        print(
            f"Applying temporary Mode {mode} date color "
            f"#{test_color} through CP."
        )
        save_palette(client, mode, test_palette, test_delimiter_color)

        changed = read_palette(client, mode)
        actual_color = changed.entries.get(TEST_DATE_ROLE)
        if actual_color != test_color:
            raise PaletteTestError(
                f"LP readback for role 02 returned {actual_color}, "
                f"expected {test_color}"
            )
        print(f"  Verified role 02 readback as #{test_color}.")

        restore_defaults(client, mode)
        cleanup_required = False

        restored = read_palette(client, mode)
        verify_factory_palette(restored)
        print(f"  Verified Mode {mode} factory defaults after DP.")
    finally:
        if cleanup_required:
            attempt_cleanup(
                client,
                host,
                port,
                client.board_id,
                timeout,
                mode,
            )


def run_no_write_flow(client: PaletteTCPClient) -> None:
    print("\nRunning read-only --no-write flow.")
    for mode in (1, 2, 3):
        response = read_palette(client, mode)
        print_factory_comparison(response)
    read_palette(client, 4, expected_status=0x01)


def run_full_test(
    client: PaletteTCPClient,
    host: str,
    port: int,
    timeout: float,
    test_delimiter_color: bool,
) -> None:
    print("\nRunning full palette test flow.")
    run_temporary_save_test(
        client,
        host,
        port,
        timeout,
        mode=1,
        test_delimiter_color=test_delimiter_color,
    )

    for mode in (2, 3):
        response = read_palette(client, mode)
        print_factory_comparison(response)
    read_palette(client, 4, expected_status=0x01)


def run_defaults_action(client: PaletteTCPClient, mode: int) -> None:
    restore_defaults(client, mode)
    response = read_palette(client, mode)
    verify_factory_palette(response)
    print(f"Verified Mode {mode} factory defaults.")


def run_rc_smoke_test(client: PaletteTCPClient) -> None:
    request = build_prefix(client.board_id, b"RC") + b"\\"
    client.send(request, "RC smoke test")
    response = client.read_exact(17, "RC response")
    print_frame("RX RC smoke test", response)
    validate_response_prefix(response, client.board_id, b"rc")
    if len(response) != 17:
        raise PaletteTestError(f"RC response length is {len(response)}, expected 17")

    print(
        "  RC parsed: "
        f"memory_written={response[6]} format={response[7]} "
        f"intensity={response[8]} "
        f"time={response[11]:02d}:{response[10]:02d}:{response[9]:02d} "
        f"date={response[13]:02d}-{response[14]:02d}-{response[15]:02d} "
        f"weekday={response[12]}"
    )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Test the ESP32 Color Palette LP/CP/DP protocol over TCP. "
            "Run only after flashing palette-capable firmware."
        )
    )
    parser.add_argument("--host", required=True, help="ESP32 IPv4 address or hostname")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port (default: 5000)")
    parser.add_argument(
        "--board-id",
        type=parse_byte_argument,
        default=0,
        help="raw one-byte board ID in decimal or 0xNN form (default: 0)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="socket timeout in seconds (default: 5)",
    )
    parser.add_argument(
        "--mode",
        type=int,
        choices=(1, 2, 3),
        default=1,
        help="mode used by read/save-test/defaults actions (default: 1)",
    )
    parser.add_argument(
        "--no-write",
        action="store_true",
        help="read Modes 1-3 and verify Mode 4 rejection without CP or DP",
    )
    parser.add_argument(
        "--test-delimiter-color",
        action="store_true",
        help="temporarily test ASCII color 005CFF, then restore defaults",
    )
    parser.add_argument(
        "--test-rc",
        action="store_true",
        help="also perform a non-destructive legacy RC command smoke test",
    )
    parser.add_argument(
        "action",
        nargs="?",
        choices=("full-test", "read", "save-test", "defaults"),
        default="full-test",
        help="operation to run (default: full-test)",
    )
    return parser


def validate_arguments(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.board_id == TERMINATOR:
        parser.error("--board-id 0x5C is invalid because it is the frame terminator")
    if args.no_write and args.action not in ("full-test", "read"):
        parser.error("--no-write cannot be combined with save-test or defaults")
    if args.no_write and args.test_delimiter_color:
        parser.error("--test-delimiter-color writes a palette and cannot use --no-write")
    if args.test_delimiter_color and args.action in ("read", "defaults"):
        parser.error("--test-delimiter-color requires full-test or save-test")


def main() -> int:
    parser = build_argument_parser()
    args = parser.parse_args()
    validate_arguments(parser, args)

    try:
        with PaletteTCPClient(
            args.host,
            args.port,
            args.board_id,
            args.timeout,
        ) as client:
            if args.test_rc:
                run_rc_smoke_test(client)

            if args.no_write:
                run_no_write_flow(client)
            elif args.action == "full-test":
                run_full_test(
                    client,
                    args.host,
                    args.port,
                    args.timeout,
                    args.test_delimiter_color,
                )
            elif args.action == "read":
                response = read_palette(client, args.mode)
                print_factory_comparison(response)
            elif args.action == "save-test":
                run_temporary_save_test(
                    client,
                    args.host,
                    args.port,
                    args.timeout,
                    args.mode,
                    args.test_delimiter_color,
                )
            else:
                run_defaults_action(client, args.mode)
    except KeyboardInterrupt:
        print("\nRESULT: FAIL - interrupted by user", file=sys.stderr)
        return 130
    except PaletteTestError as exc:
        print(f"\nRESULT: FAIL - {exc}", file=sys.stderr)
        return 1

    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

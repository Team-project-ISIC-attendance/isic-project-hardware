#!/usr/bin/env python3
"""
ESP Filesystem Inspector Tool

This tool allows you to inspect the LittleFS filesystem on your ESP device.
You can list files, read file contents, and monitor log files in real-time.

Usage:
    python esp_fs_inspector.py --port /dev/ttyUSB0 list
    python esp_fs_inspector.py --port /dev/ttyUSB0 read /config.json
    python esp_fs_inspector.py --port /dev/ttyUSB0 cat /config.json
    python esp_fs_inspector.py --port /dev/ttyUSB0 tail /logs/device.log
    python esp_fs_inspector.py --port /dev/ttyUSB0 info
    python esp_fs_inspector.py --port /dev/ttyUSB0 terminal  # Interactive mode
"""

import argparse
import json
import sys
import time
import shlex
from typing import Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("The 'pyserial' package is required. Install it via 'pip install pyserial'.", file=sys.stderr)
    sys.exit(1)


class ESPFilesystemInspector:
    """Inspector for ESP LittleFS filesystem over serial connection."""

    COMMAND_PREFIX = "FS_CMD:"
    RESPONSE_PREFIX = "FS_RESP:"
    RESPONSE_END = "FS_END"

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 2.0):
        """Initialize the inspector with serial connection parameters."""
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial: Optional[serial.Serial] = None

    def connect(self) -> bool:
        """Connect to the ESP device."""
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=self.timeout
            )
            time.sleep(0.5)  # Give device time to reset if needed
            print(f"Connected to {self.port} at {self.baudrate} baud")
            return True
        except serial.SerialException as e:
            print(f"Error connecting to {self.port}: {e}", file=sys.stderr)
            return False

    def disconnect(self):
        """Disconnect from the ESP device."""
        if self.serial and self.serial.is_open:
            self.serial.close()
            print("Disconnected")

    def send_command(self, command: str) -> Optional[str]:
        """Send a command to the ESP and wait for response."""
        if not self.serial or not self.serial.is_open:
            print("Error: Not connected to device", file=sys.stderr)
            return None

        # Clear any pending input
        self.serial.reset_input_buffer()

        # Send command
        cmd_line = f"{self.COMMAND_PREFIX}{command}\n"
        self.serial.write(cmd_line.encode('utf-8'))
        self.serial.flush()

        # Wait for response
        response_lines = []
        start_time = time.time()
        found_response = False

        while time.time() - start_time < self.timeout:
            if self.serial.in_waiting:
                line = self.serial.readline().decode('utf-8', errors='ignore').strip()

                if line.startswith(self.RESPONSE_PREFIX):
                    # Remove prefix and store
                    response_lines.append(line[len(self.RESPONSE_PREFIX):])
                    found_response = True
                elif line == self.RESPONSE_END and found_response:
                    # End of response
                    return '\n'.join(response_lines)

        if not found_response:
            print("Error: No response from device (command may not be implemented)", file=sys.stderr)
            print("Tip: Make sure your firmware includes the filesystem command handlers", file=sys.stderr)

        return None

    def list_files(self, path: str = "/") -> bool:
        """List all files in the filesystem."""
        print(f"Listing files in '{path}'...")
        response = self.send_command(f"LIST {path}")

        if response:
            print("\nFiles:")
            print(response)
            return True
        return False

    def read_file(self, filepath: str) -> Optional[str]:
        """Read and return file contents."""
        response = self.send_command(f"READ {filepath}")
        return response

    def cat_file(self, filepath: str) -> bool:
        """Read and display file contents."""
        print(f"Reading '{filepath}'...")
        content = self.read_file(filepath)

        if content:
            print("\n" + "=" * 60)
            print(f"Content of {filepath}:")
            print("=" * 60)

            # Try to pretty-print JSON
            if filepath.endswith('.json'):
                try:
                    parsed = json.loads(content)
                    print(json.dumps(parsed, indent=2))
                except json.JSONDecodeError:
                    print(content)
            else:
                print(content)

            print("=" * 60)
            return True
        return False

    def tail_file(self, filepath: str, lines: int = 10) -> bool:
        """Display last N lines of a file."""
        print(f"Tailing last {lines} lines of '{filepath}'...")
        content = self.read_file(filepath)

        if content:
            all_lines = content.split('\n')
            tail_lines = all_lines[-lines:]

            print("\n" + "=" * 60)
            print(f"Last {lines} lines of {filepath}:")
            print("=" * 60)
            print('\n'.join(tail_lines))
            print("=" * 60)
            return True
        return False

    def get_info(self) -> bool:
        """Get filesystem information."""
        print("Getting filesystem info...")
        response = self.send_command("INFO")

        if response:
            print("\nFilesystem Information:")
            print(response)
            return True
        return False

    def monitor(self, filepath: str, interval: float = 1.0):
        """Monitor a file for changes (like tail -f)."""
        print(f"Monitoring '{filepath}' (Ctrl+C to stop)...")
        last_content = ""

        try:
            while True:
                content = self.read_file(filepath)
                if content and content != last_content:
                    # Show only new lines
                    if last_content:
                        new_lines = content[len(last_content):]
                        if new_lines:
                            print(new_lines, end='')
                    else:
                        print(content)
                    last_content = content

                time.sleep(interval)
        except KeyboardInterrupt:
            print("\nMonitoring stopped")

    def interactive_terminal(self):
        """Run an interactive terminal session."""
        print("\n" + "=" * 70)
        print("ESP Filesystem Inspector - Interactive Terminal")
        print("=" * 70)
        print("\nAvailable commands:")
        print("  ls [path]              - List files in directory (default: /)")
        print("  cat <file>             - Display file contents")
        print("  read <file>            - Read file contents (raw)")
        print("  tail <file> [lines]    - Display last N lines (default: 10)")
        print("  info                   - Show filesystem information")
        print("  help                   - Show this help message")
        print("  exit, quit             - Exit terminal")
        print("\nType 'help' for this message anytime.")
        print("=" * 70 + "\n")

        while True:
            try:
                # Read user input
                user_input = input("esp-fs> ").strip()

                if not user_input:
                    continue

                # Parse command
                try:
                    parts = shlex.split(user_input)
                except ValueError as e:
                    print(f"Error parsing command: {e}")
                    continue

                cmd = parts[0].lower()
                args = parts[1:]

                # Handle commands
                if cmd in ['exit', 'quit', 'q']:
                    print("Exiting terminal...")
                    break

                elif cmd in ['help', '?']:
                    print("\nAvailable commands:")
                    print("  ls [path]              - List files in directory (default: /)")
                    print("  cat <file>             - Display file contents")
                    print("  read <file>            - Read file contents (raw)")
                    print("  tail <file> [lines]    - Display last N lines (default: 10)")
                    print("  info                   - Show filesystem information")
                    print("  help                   - Show this help message")
                    print("  exit, quit             - Exit terminal\n")

                elif cmd in ['ls', 'list']:
                    path = args[0] if args else "/"
                    self.list_files(path)

                elif cmd == 'cat':
                    if not args:
                        print("Error: cat requires a file path")
                        print("Usage: cat <file>")
                        continue
                    self.cat_file(args[0])

                elif cmd == 'read':
                    if not args:
                        print("Error: read requires a file path")
                        print("Usage: read <file>")
                        continue
                    content = self.read_file(args[0])
                    if content:
                        print(content)

                elif cmd == 'tail':
                    if not args:
                        print("Error: tail requires a file path")
                        print("Usage: tail <file> [lines]")
                        continue
                    filepath = args[0]
                    lines = int(args[1]) if len(args) > 1 else 10
                    self.tail_file(filepath, lines)

                elif cmd == 'info':
                    self.get_info()

                else:
                    print(f"Unknown command: {cmd}")
                    print("Type 'help' for available commands")

            except KeyboardInterrupt:
                print("\n\nUse 'exit' or 'quit' to leave the terminal")
                continue
            except EOFError:
                print("\nExiting terminal...")
                break
            except Exception as e:
                print(f"Error: {e}")


def list_serial_ports():
    """List available serial ports."""
    ports = list_ports.comports()
    if not ports:
        print("No serial ports found")
        return

    print("Available serial ports:")
    for port in ports:
        print(f"  {port.device} - {port.description}")


def main():
    parser = argparse.ArgumentParser(
        description="ESP Filesystem Inspector - Inspect LittleFS on your ESP device",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --port /dev/ttyUSB0 list
  %(prog)s --port /dev/cu.usbserial-0001 read /config.json
  %(prog)s --port COM3 cat /config.json
  %(prog)s --port /dev/ttyUSB0 tail /logs/device.log --lines 20
  %(prog)s --port /dev/ttyUSB0 monitor /logs/device.log
  %(prog)s --port /dev/ttyUSB0 terminal
  %(prog)s --list-ports
        """
    )

    parser.add_argument(
        '--port', '-p',
        help='Serial port (e.g., /dev/ttyUSB0, COM3, /dev/cu.usbserial-*)'
    )
    parser.add_argument(
        '--baudrate', '-b',
        type=int,
        default=115200,
        help='Baud rate (default: 115200)'
    )
    parser.add_argument(
        '--timeout', '-t',
        type=float,
        default=2.0,
        help='Command timeout in seconds (default: 2.0)'
    )
    parser.add_argument(
        '--list-ports',
        action='store_true',
        help='List available serial ports and exit'
    )

    subparsers = parser.add_subparsers(dest='command', help='Command to execute')

    # List command
    list_parser = subparsers.add_parser('list', help='List files in filesystem')
    list_parser.add_argument('path', nargs='?', default='/', help='Path to list (default: /)')

    # Read command
    read_parser = subparsers.add_parser('read', help='Read file contents (raw)')
    read_parser.add_argument('filepath', help='Path to file')

    # Cat command
    cat_parser = subparsers.add_parser('cat', help='Display file contents (formatted)')
    cat_parser.add_argument('filepath', help='Path to file')

    # Tail command
    tail_parser = subparsers.add_parser('tail', help='Display last N lines of file')
    tail_parser.add_argument('filepath', help='Path to file')
    tail_parser.add_argument('--lines', '-n', type=int, default=10, help='Number of lines (default: 10)')

    # Monitor command
    monitor_parser = subparsers.add_parser('monitor', help='Monitor file for changes (like tail -f)')
    monitor_parser.add_argument('filepath', help='Path to file')
    monitor_parser.add_argument('--interval', '-i', type=float, default=1.0,
                                help='Check interval in seconds (default: 1.0)')

    # Info command
    info_parser = subparsers.add_parser('info', help='Get filesystem information')

    # Terminal command
    terminal_parser = subparsers.add_parser('terminal', help='Interactive terminal mode')

    args = parser.parse_args()

    # List ports if requested
    if args.list_ports:
        list_serial_ports()
        return 0

    # Validate arguments
    if not args.command:
        parser.print_help()
        return 1

    if not args.port:
        print("Error: --port is required", file=sys.stderr)
        print("\nUse --list-ports to see available ports", file=sys.stderr)
        return 1

    # Create inspector and connect
    inspector = ESPFilesystemInspector(args.port, args.baudrate, args.timeout)

    if not inspector.connect():
        return 1

    try:
        # Execute command
        success = False

        if args.command == 'list':
            success = inspector.list_files(args.path)
        elif args.command == 'read':
            content = inspector.read_file(args.filepath)
            if content:
                print(content)
                success = True
        elif args.command == 'cat':
            success = inspector.cat_file(args.filepath)
        elif args.command == 'tail':
            success = inspector.tail_file(args.filepath, args.lines)
        elif args.command == 'monitor':
            inspector.monitor(args.filepath, args.interval)
            success = True
        elif args.command == 'info':
            success = inspector.get_info()
        elif args.command == 'terminal':
            inspector.interactive_terminal()
            success = True

        return 0 if success else 1

    finally:
        inspector.disconnect()


if __name__ == '__main__':
    sys.exit(main())

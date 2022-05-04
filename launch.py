import serial
import time
import argparse
import sys

DEBUG = True

# Call this program with
# python coa202ser.py portdevice
# e.g. on windows:
#      python coa202ser.py COM2
# or macos/linux:
#      python coa202ser.py /dev/ttyACM0
#
# Remember you cannot leave this running while uploading
# Check lab worksheet 5 to set up your environment

# The code below parses the command line arguments and leaves the
# variable args.port containing the device name.

parser = argparse.ArgumentParser(
	description='Talk to Ardunio over serial')
parser.add_argument(
	'port',
	metavar='device',
	type=str,
	nargs='?',
	help='The port to connect to')
args = parser.parse_args()

# Open the serial port (this will reset your Arduino)
print('connecting to port', args.port)
ser = serial.Serial(args.port, 9600, timeout=1, # 0.25
					rtscts=False, xonxoff=False, dsrdtr=False)

print("waiting for sync")
going = True
while going:
	s = ser.read(1)  # Read just one byte
	print(s)       # Print it for debugging
	if s == b'Q':
		going = False
ser.write(b'X')
print("Sync")

line = ser.readline()
print(line)  # This should print BASIC or your extension list


# Build a list of messages to send
# the b'' notation creates byte arrays suitable for
# passing to ser.write().  ser.write() will not accept
# str variables.
print("SETUP COMPLETE")
msgs = [
	"CAFirst",
	"XA5",
	"VA10",
	"VA15\nVA20",

	"CBSecondary",
	"VB15",
	"CCThird",
	"NC25",
	"VC20",

	"CDFourth",
	"XD20",
	"VD25",

	# "XA25",
	# "VA50",
	# "VA100",
	# "VA150",
	# "VB5",
	# "VA200",
	# "VA250",
	# "VB4",
	# "XC100",
	# "NC50",
	# "VC0",
	# "XD50",
	# "VD70",
	# "VD250"
]
import string
# msgs = [f"C{letter}{letter} Channel {letter}" for letter in string.ascii_uppercase]
# msgs.extend((f"V{letter}{val}" for val in range(1, 100) for letter in string.ascii_uppercase))

# msgs = [
# 	"CAC A",
# 	*(f"VA{i}" for i in range(1, 100)),
# ]

# msgs = [j for i in range(26) if (x:=chr(65+i)) for j in [f"C{x}Channel {x}", *(f"V{x}{v}" for v in range(1, 43))]]


# Simply write these messages out once per second
# Customise above and below as you see fit.

def reading():
	# Check for message back.  This will timeout after a second
	line = True
	while line:
		line = ser.readline()
		if line:
			print(line)

for x in msgs:
	reading()
	print("> WRT:", str(x))
	ser.write(x.encode()+b'\n')
	reading()
print("""##########\nSENDING DONE\n##########""")
while True:
	reading()

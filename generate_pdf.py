#!/usr/bin/env python3
"""Generate a PDF documenting the run commands for AlphaStar_LDD."""

from fpdf import FPDF


class RunCommandsPDF(FPDF):
    def header(self):
        self.set_font("Helvetica", "B", 18)
        self.cell(0, 12, "AlphaStar LDD - Run Commands", new_x="LMARGIN", new_y="NEXT", align="C")
        self.set_font("Helvetica", "", 10)
        self.cell(0, 6, "JHD162A 16x2 LCD Character Device Driver for Raspberry Pi 4", new_x="LMARGIN", new_y="NEXT", align="C")
        self.ln(4)
        self.line(10, self.get_y(), 200, self.get_y())
        self.ln(4)

    def footer(self):
        self.set_y(-15)
        self.set_font("Helvetica", "I", 8)
        self.cell(0, 10, f"Page {self.page_no()}/{{nb}}", align="C")

    def section_title(self, title):
        self.set_font("Helvetica", "B", 14)
        self.set_fill_color(230, 230, 230)
        self.cell(0, 10, title, new_x="LMARGIN", new_y="NEXT", fill=True)
        self.ln(3)

    def body_text(self, text):
        self.set_font("Helvetica", "", 11)
        self.multi_cell(0, 6, text)
        self.ln(2)

    def code_block(self, code):
        self.set_font("Courier", "", 10)
        self.set_fill_color(245, 245, 245)
        self.set_draw_color(200, 200, 200)
        x = self.get_x()
        y = self.get_y()
        # Calculate height needed
        lines = code.strip().split("\n")
        block_h = len(lines) * 6 + 6
        self.rect(x, y, 190, block_h)
        self.set_xy(x + 3, y + 3)
        for line in lines:
            self.cell(0, 6, line, new_x="LMARGIN", new_y="NEXT")
            self.set_x(x + 3)
        self.set_xy(x, y + block_h + 3)

    def bullet(self, text):
        self.set_font("Helvetica", "", 11)
        x = self.get_x()
        self.set_x(x + 5)
        self.cell(5, 6, "-")
        self.multi_cell(175, 6, text)
        self.ln(1)


pdf = RunCommandsPDF()
pdf.alias_nb_pages()
pdf.set_auto_page_break(auto=True, margin=20)
pdf.add_page()

# Prerequisites
pdf.section_title("Prerequisites")
pdf.body_text(
    "Before building and running the driver, ensure the following:"
)
pdf.bullet("Raspberry Pi 4 running Linux (Raspberry Pi OS recommended)")
pdf.bullet(
    "Linux kernel headers installed. Install with:\n"
    "  sudo apt install raspberrypi-kernel-headers"
)
pdf.bullet(
    "LCD wired in 4-bit mode to the following GPIO pins:\n"
    "  RS  -> GPIO26 (Pin 37)\n"
    "  E   -> GPIO19 (Pin 35)\n"
    "  D4  -> GPIO13 (Pin 33)\n"
    "  D5  -> GPIO6  (Pin 31)\n"
    "  D6  -> GPIO5  (Pin 29)\n"
    "  D7  -> GPIO20 (Pin 38)\n"
    "  RW  -> GND    (Pin 9)  [write-only mode]"
)
pdf.ln(3)

# Build
pdf.section_title("1. Build the Kernel Module")
pdf.body_text(
    "Compile pcd.c into the kernel module pcd.ko using the provided Makefile:"
)
pdf.code_block("make")
pdf.body_text(
    "This invokes the kernel build system at /lib/modules/$(uname -r)/build "
    "to compile the out-of-tree module."
)

# Load
pdf.section_title("2. Load the Module")
pdf.body_text("Insert the compiled kernel module:")
pdf.code_block("sudo insmod pcd.ko")
pdf.body_text(
    'This initializes the LCD hardware via GPIO, displays "AlphaStar" on the screen, '
    "and creates the character device at /dev/lcd_jhd162a."
)

# Use
pdf.section_title("3. Use the LCD Device")

pdf.body_text("Write text to the display (single line):")
pdf.code_block('echo "Hello World" > /dev/lcd_jhd162a')

pdf.body_text("Write two lines (split by newline):")
pdf.code_block('printf "Line 1\\nLine 2" > /dev/lcd_jhd162a')

pdf.body_text('Read from the device (returns "AlphaStar"):')
pdf.code_block("cat /dev/lcd_jhd162a")

# Unload
pdf.section_title("4. Unload the Module")
pdf.body_text("Remove the driver, which clears the LCD and frees GPIO resources:")
pdf.code_block("sudo rmmod pcd")

# Clean
pdf.section_title("5. Clean Build Artifacts")
pdf.body_text("Remove all compiled files:")
pdf.code_block("make clean")

# Check logs
pdf.section_title("6. View Kernel Logs (Troubleshooting)")
pdf.body_text("Check kernel messages for driver status and errors:")
pdf.code_block("dmesg | grep lcd")

# Quick reference
pdf.section_title("Quick Reference")
pdf.set_font("Courier", "", 10)
pdf.set_fill_color(245, 245, 245)
ref = [
    ("Build module", "make"),
    ("Load driver", "sudo insmod pcd.ko"),
    ("Write to LCD", 'echo "text" > /dev/lcd_jhd162a'),
    ("Read from LCD", "cat /dev/lcd_jhd162a"),
    ("Unload driver", "sudo rmmod pcd"),
    ("Clean build", "make clean"),
    ("Check logs", "dmesg | grep lcd"),
]
pdf.set_font("Helvetica", "B", 10)
pdf.set_fill_color(200, 200, 200)
pdf.cell(70, 8, "  Action", border=1, fill=True)
pdf.cell(120, 8, "  Command", border=1, fill=True, new_x="LMARGIN", new_y="NEXT")
pdf.set_font("Courier", "", 10)
pdf.set_fill_color(255, 255, 255)
for action, cmd in ref:
    pdf.set_font("Helvetica", "", 10)
    pdf.cell(70, 8, "  " + action, border=1)
    pdf.set_font("Courier", "", 10)
    pdf.cell(120, 8, "  " + cmd, border=1, new_x="LMARGIN", new_y="NEXT")

output_path = "/home/user/AlphaStar_LDD/AlphaStar_LDD_Run_Commands.pdf"
pdf.output(output_path)
print(f"PDF generated: {output_path}")

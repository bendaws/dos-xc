# OpenWork

OpenWork is the office suite in the `openwork` package (`\Programs\openwork`).
Its four programs share one look, modelled on Office 2013: a ribbon of tabs,
a File view (Info, New, Open, Save, Save As, Close, About), an accent-coloured
status bar and one About dialog that names the program it was opened from.

| Program | File | Accent | Opens and saves |
|---------|------|--------|-----------------|
| OpenPapers | `PAPERS.EXE` | blue | `.docx`, `.odt`, `.rtf`, `.html`, `.md`, `.txt` |
| OpenNumbers | `NUMBERS.EXE` | green | `.xlsx`, `.ods`, `.csv`, `.tsv` |
| OpenShow | `SHOW.EXE` | red | `.pptx`, `.odp` (opens `.ppsx` too) |
| OpenMail | `MAIL.EXE` | yellow | email over IMAP or POP3, sent with SMTP |

Files open from the command line (`PAPERS letter.docx`) or by double-clicking
them.  Recent files are listed under File > Open; they are kept in
`%HOME%\OPENWORK.CFG`.  Google Drive shortcuts (`.gdoc`, `.gsheet`,
`.gslides`) open in the default browser.

The sources are in [`/openwork`](../openwork): `suite.c` holds the shared
ribbon, File view, About dialog and controls; `icons.py` draws the program
icons and the ribbon icons (`RIBBON16.PNG`, `RIBBON32.PNG`) when the package
is built.

## Keys

| Key | Action |
|-----|--------|
| Ctrl+N, Ctrl+O, Ctrl+S | New, File > Open, Save |
| Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+A | Copy, cut, paste, select all |
| Ctrl+F, F3 | Find, find next (OpenPapers, OpenNumbers) |
| Ctrl+B, Ctrl+U | Bold, underline (OpenPapers) |
| Ctrl+L, Ctrl+E, Ctrl+R | Align left, centre, right (OpenPapers) |
| F2 | Edit the cell (OpenNumbers) |
| F5, Shift+F5 | Slide show from the beginning, from this slide (OpenShow) |
| F5 | Send/Receive (OpenMail) |
| F1 | About |
| Esc | Leave the File view |

## OpenShow

Slides are 16:9 with five layouts: Title Slide, Title and Content, Section
Header, Title Only and Blank.  In a bulleted text box, Tab and Shift+Tab
change the list level.  The Design tab has six colour themes; the theme is
saved as each slide's background and text colours.  Speaker notes are saved
in `.pptx` and `.odp` files.  The slide show fills the screen and hides the
taskbar; click, Space or the arrow keys move between slides, and Esc ends it.

Pictures, shapes, tables, charts and animations in a presentation are not
shown, and they are lost if the file is saved again.

## OpenMail

The first start opens Account Settings.  Common providers (Gmail, Outlook.com,
Yahoo, iCloud, AOL, Fastmail) are filled in from the email address; for
others, OpenMail guesses `imap.` and `smtp.` servers on the address's domain.
Providers that require two-step sign-in need an app password.

- IMAP shows every folder.  Opening a message marks it read; Delete moves it
  to the Trash folder (or deletes it there); sent messages are copied to the
  Sent folder.  The folder refreshes every ten minutes.
- POP3 shows the Inbox only and leaves messages on the server until deleted.
- The newest 200 messages of a folder are listed.
- Messages are shown as plain text; HTML-only messages are converted to text.
  Click an attachment to save it.  Attach File adds attachments to a new
  message.

Connections use SSL/TLS or STARTTLS through OpenSSL and check the server's
certificate against `CACERT.PEM` in the package folder, so the computer's
clock must be right (see [NETWORK.md](NETWORK.md#tls)).  If the package was
built without the OpenSSL from the ports build, only unencrypted connections
work.

The account is kept in `%HOME%\OPENMAIL.CFG`.  The password there is only
encoded (base64), not encrypted, so anyone who can read that file can read
the password.

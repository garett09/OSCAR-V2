#!/usr/bin/env python3
"""Generate trimmed Qt dialog translation catalogs for OSCAR."""

from __future__ import annotations

from pathlib import Path
import re
import xml.etree.ElementTree as ET


REPO_ROOT = Path(__file__).resolve().parents[1]
TRANSLATIONS_DIR = REPO_ROOT / "Translations"
OUTPUT_DIR = TRANSLATIONS_DIR / "qt"
QT_TRANSLATIONS_DIR = Path(r"C:\Qt\6.10.2\Src\qttranslations\translations")

TEMPLATE_CONTEXTS = ("QDialogButtonBox", "QFileDialog", "QPlatformTheme")
TEMPLATE_FILE = QT_TRANSLATIONS_DIR / "qtbase_de.ts"

QT_CODE_MAP = {
    "ar": ["ar"],
    "bg": ["bg"],
    "cz": ["cs"],
    "da": ["da"],
    "de": ["de"],
    "es": ["es"],
    "es_MX": ["es"],
    "fi": ["fi"],
    "fr": ["fr"],
    "he": ["he"],
    "hu": ["hu"],
    "it": ["it"],
    "ja": ["ja"],
    "ko": ["ko"],
    "nl": ["nl"],
    "pl": ["pl"],
    "pt": ["pt_PT"],
    "pt_BR": ["pt_BR"],
    "ru": ["ru"],
    "sv": ["sv"],
    "tr": ["tr"],
    "uk": ["uk"],
    "zh_CN": ["zh_CN"],
    "zh_TW": ["zh_TW"],
}

MANUAL_OVERRIDES = {
    "af": {
        "&Choose": "&Kies",
        "&Delete": "&Skrap",
        "&New Folder": "Nuwe &gids",
        "&No": "&Nee",
        "&Open": "&Maak oop",
        "&Rename": "&Hernoem",
        "&Save": "&Stoor",
        "&Yes": "&Ja",
        "Abort": "Staak",
        "All Files (*)": "Alle lêers (*)",
        "All files (*)": "Alle lêers (*)",
        "Apply": "Pas toe",
        "Back": "Terug",
        "Cancel": "Kanselleer",
        "Change to detail view mode": "Skakel na detailaansig",
        "Change to list view mode": "Skakel na lysaansig",
        "Close": "Sluit",
        "Close without Saving": "Sluit sonder om te stoor",
        "Create New Folder": "Skep nuwe gids",
        "Create a New Folder": "Skep 'n nuwe gids",
        "Detail View": "Detailaansig",
        "Directories": "Gidse",
        "Directory:": "Gids:",
        "Discard": "Verwerp",
        "Don't Save": "Moenie stoor nie",
        "File &name:": "Lêer&naam:",
        "Files": "Lêers",
        "Files of type:": "Lêers van tipe:",
        "Find Directory": "Vind gids",
        "Forward": "Vorentoe",
        "Go back": "Gaan terug",
        "Go forward": "Gaan vorentoe",
        "Go to the parent directory": "Gaan na die ouer gids",
        "Help": "Hulp",
        "Ignore": "Ignoreer",
        "List View": "Lysaansig",
        "List of places and bookmarks": "Lys van plekke en boekmerke",
        "Look in:": "Kyk in:",
        "N&o to All": "Ne&e vir alles",
        "New Folder": "Nuwe gids",
        "OK": "OK",
        "Open": "Maak oop",
        "Parent Directory": "Ouer gids",
        "Reset": "Herstel",
        "Remove": "Verwyder",
        "Recent Places": "Onlangse plekke",
        "Restore Defaults": "Herstel verstekwaardes",
        "Retry": "Probeer weer",
        "Save": "Stoor",
        "Save All": "Stoor alles",
        "Save As": "Stoor as",
        "Show ": "Wys ",
        "Show &hidden files": "Wys &versteekte lêers",
        "Sidebar": "Sybalk",
        "Yes to &All": "Ja vir &alles",
        "%1\nDirectory not found.\nPlease verify the correct directory name was given.": "%1\nGids nie gevind nie.\nMaak seker die korrekte gidsnaam is gegee.",
        "%1 already exists.\nDo you want to replace it?": "%1 bestaan reeds.\nWil jy dit vervang?",
        "%1\nFile not found.\nPlease verify the correct file name was given.": "%1\nLêer nie gevind nie.\nMaak seker die korrekte lêernaam is gegee.",
        "'%1' is write protected.\nDo you want to delete it anyway?": "'%1' is skryfbeskerm.\nWil jy dit steeds skrap?",
        "Are you sure you want to delete '%1'?": "Is jy seker jy wil '%1' skrap?",
        "Could not delete directory.": "Kon nie gids skrap nie.",
        "Delete": "Skrap",
    },
    "el": {
        "&Choose": "&Επιλογή",
        "&Delete": "&Διαγραφή",
        "&New Folder": "Νέος &φάκελος",
        "&No": "&Όχι",
        "&Open": "&Άνοιγμα",
        "&Rename": "&Μετονομασία",
        "&Save": "&Αποθήκευση",
        "&Yes": "&Ναι",
        "Abort": "Ματαίωση",
        "All Files (*)": "Όλα τα αρχεία (*)",
        "All files (*)": "Όλα τα αρχεία (*)",
        "Apply": "Εφαρμογή",
        "Back": "Πίσω",
        "Cancel": "Ακύρωση",
        "Change to detail view mode": "Αλλαγή σε προβολή λεπτομερειών",
        "Change to list view mode": "Αλλαγή σε προβολή λίστας",
        "Close": "Κλείσιμο",
        "Close without Saving": "Κλείσιμο χωρίς αποθήκευση",
        "Create New Folder": "Δημιουργία νέου φακέλου",
        "Create a New Folder": "Δημιουργία ενός νέου φακέλου",
        "Detail View": "Προβολή λεπτομερειών",
        "Directories": "Κατάλογοι",
        "Directory:": "Κατάλογος:",
        "Discard": "Απόρριψη",
        "Don't Save": "Να μην αποθηκευτεί",
        "File &name:": "Όνομα &αρχείου:",
        "Files": "Αρχεία",
        "Files of type:": "Αρχεία τύπου:",
        "Find Directory": "Εύρεση καταλόγου",
        "Forward": "Μπροστά",
        "Go back": "Μετάβαση πίσω",
        "Go forward": "Μετάβαση μπροστά",
        "Go to the parent directory": "Μετάβαση στον γονικό κατάλογο",
        "Help": "Βοήθεια",
        "Ignore": "Παράβλεψη",
        "List View": "Προβολή λίστας",
        "List of places and bookmarks": "Λίστα τοποθεσιών και σελιδοδεικτών",
        "Look in:": "Αναζήτηση σε:",
        "N&o to All": "Όχ&ι σε όλα",
        "New Folder": "Νέος φάκελος",
        "OK": "OK",
        "Open": "Άνοιγμα",
        "Parent Directory": "Γονικός κατάλογος",
        "Reset": "Επαναφορά",
        "Recent Places": "Πρόσφατες τοποθεσίες",
        "Remove": "Αφαίρεση",
        "Restore Defaults": "Επαναφορά προεπιλογών",
        "Retry": "Επανάληψη",
        "Save": "Αποθήκευση",
        "Save All": "Αποθήκευση όλων",
        "Save As": "Αποθήκευση ως",
        "Show ": "Εμφάνιση ",
        "Show &hidden files": "Εμφάνιση &κρυφών αρχείων",
        "Sidebar": "Πλευρική γραμμή",
        "Yes to &All": "Ναι σε ό&λα",
        "%1\nDirectory not found.\nPlease verify the correct directory name was given.": "%1\nΔεν βρέθηκε ο κατάλογος.\nΠαρακαλώ βεβαιωθείτε ότι δόθηκε το σωστό όνομα καταλόγου.",
        "%1 already exists.\nDo you want to replace it?": "Το %1 υπάρχει ήδη.\nΘέλετε να το αντικαταστήσετε;",
        "%1\nFile not found.\nPlease verify the correct file name was given.": "%1\nΔεν βρέθηκε το αρχείο.\nΠαρακαλώ βεβαιωθείτε ότι δόθηκε το σωστό όνομα αρχείου.",
        "'%1' is write protected.\nDo you want to delete it anyway?": "Το '%1' είναι προστατευμένο από εγγραφή.\nΘέλετε να το διαγράψετε ούτως ή άλλως;",
        "Are you sure you want to delete '%1'?": "Είστε βέβαιοι ότι θέλετε να διαγράψετε το '%1';",
        "Could not delete directory.": "Δεν ήταν δυνατή η διαγραφή του καταλόγου.",
        "Delete": "Διαγραφή",
    },
    "en_UK": {},
    "fil": {
        "&Choose": "&Piliin",
        "&Delete": "&Tanggalin",
        "&New Folder": "Bagong &Folder",
        "&No": "&Hindi",
        "&Open": "&Buksan",
        "&Rename": "&Palitan ang Pangalan",
        "&Save": "&I-save",
        "&Yes": "&Oo",
        "Abort": "Ihinto",
        "All Files (*)": "Lahat ng File (*)",
        "All files (*)": "Lahat ng file (*)",
        "Apply": "Ilapat",
        "Back": "Bumalik",
        "Cancel": "Kanselahin",
        "Change to detail view mode": "Lumipat sa detalyadong view",
        "Change to list view mode": "Lumipat sa listahang view",
        "Close": "Isara",
        "Close without Saving": "Isara nang Hindi Sine-save",
        "Create New Folder": "Lumikha ng Bagong Folder",
        "Create a New Folder": "Lumikha ng Isang Bagong Folder",
        "Detail View": "Detalyadong View",
        "Directories": "Mga Direktoryo",
        "Directory:": "Direktoryo:",
        "Discard": "Huwag I-save",
        "Don't Save": "Huwag I-save",
        "File &name:": "Pangalan ng &file:",
        "Files": "Mga File",
        "Files of type:": "Mga file na uri:",
        "Find Directory": "Hanapin ang Direktoryo",
        "Forward": "Pasulong",
        "Go back": "Bumalik",
        "Go forward": "Pumunta pasulong",
        "Go to the parent directory": "Pumunta sa magulang na direktoryo",
        "Help": "Tulong",
        "Ignore": "Huwag Pansinin",
        "List View": "Listahang View",
        "List of places and bookmarks": "Listahan ng mga lugar at bookmark",
        "Look in:": "Tumingin sa:",
        "N&o to All": "H&indi sa Lahat",
        "New Folder": "Bagong Folder",
        "OK": "OK",
        "Open": "Buksan",
        "Parent Directory": "Magulang na Direktoryo",
        "Reset": "I-reset",
        "Recent Places": "Mga Kamakailang Lugar",
        "Remove": "Alisin",
        "Restore Defaults": "Ibalik ang mga Default",
        "Retry": "Subukan Muli",
        "Save": "I-save",
        "Save All": "I-save Lahat",
        "Save As": "I-save Bilang",
        "Show ": "Ipakita ",
        "Show &hidden files": "Ipakita ang mga &nakatagong file",
        "Sidebar": "Sidebar",
        "Yes to &All": "Oo sa &Lahat",
        "%1\nDirectory not found.\nPlease verify the correct directory name was given.": "%1\nHindi natagpuan ang direktoryo.\nPakisuri kung tama ang pangalan ng direktoryo.",
        "%1 already exists.\nDo you want to replace it?": "Mayroon nang %1.\nGusto mo ba itong palitan?",
        "%1\nFile not found.\nPlease verify the correct file name was given.": "%1\nHindi natagpuan ang file.\nPakisuri kung tama ang pangalan ng file.",
        "'%1' is write protected.\nDo you want to delete it anyway?": "Ang '%1' ay protektado laban sa pagsusulat.\nGusto mo pa rin ba itong tanggalin?",
        "Are you sure you want to delete '%1'?": "Sigurado ka bang gusto mong tanggalin ang '%1'?",
        "Could not delete directory.": "Hindi matanggal ang direktoryo.",
        "Delete": "Tanggalin",
    },
    "no": {
        "&Choose": "&Velg",
        "&Delete": "&Slett",
        "&New Folder": "Ny &mappe",
        "&No": "&Nei",
        "&Open": "&Åpne",
        "&Rename": "&Gi nytt navn",
        "&Save": "&Lagre",
        "&Yes": "&Ja",
        "Abort": "Avbryt",
        "All Files (*)": "Alle filer (*)",
        "All files (*)": "Alle filer (*)",
        "Apply": "Bruk",
        "Back": "Tilbake",
        "Cancel": "Avbryt",
        "Change to detail view mode": "Bytt til detaljvisning",
        "Change to list view mode": "Bytt til listevisning",
        "Close": "Lukk",
        "Close without Saving": "Lukk uten a lagre",
        "Create New Folder": "Opprett ny mappe",
        "Create a New Folder": "Opprett en ny mappe",
        "Detail View": "Detaljvisning",
        "Directories": "Mapper",
        "Directory:": "Mappe:",
        "Discard": "Forkast",
        "Don't Save": "Ikke lagre",
        "File &name:": "Fil&navn:",
        "Files": "Filer",
        "Files of type:": "Filer av typen:",
        "Find Directory": "Finn mappe",
        "Forward": "Frem",
        "Go back": "Gå tilbake",
        "Go forward": "Gå frem",
        "Go to the parent directory": "Gå til overordnet mappe",
        "Help": "Hjelp",
        "Ignore": "Ignorer",
        "List View": "Listevisning",
        "List of places and bookmarks": "Liste over steder og bokmerker",
        "Look in:": "Se i:",
        "N&o to All": "N&ei til alle",
        "New Folder": "Ny mappe",
        "OK": "OK",
        "Open": "Åpne",
        "Parent Directory": "Overordnet mappe",
        "Reset": "Tilbakestill",
        "Recent Places": "Nylige steder",
        "Remove": "Fjern",
        "Restore Defaults": "Gjenopprett standarder",
        "Retry": "Prøv igjen",
        "Save": "Lagre",
        "Save All": "Lagre alle",
        "Save As": "Lagre som",
        "Show ": "Vis ",
        "Show &hidden files": "Vis &skjulte filer",
        "Sidebar": "Sidepanel",
        "Yes to &All": "Ja til &alle",
        "%1\nDirectory not found.\nPlease verify the correct directory name was given.": "%1\nFant ikke mappen.\nKontroller at riktig mappenavn ble oppgitt.",
        "%1 already exists.\nDo you want to replace it?": "%1 finnes allerede.\nVil du erstatte den?",
        "%1\nFile not found.\nPlease verify the correct file name was given.": "%1\nFant ikke filen.\nKontroller at riktig filnavn ble oppgitt.",
        "'%1' is write protected.\nDo you want to delete it anyway?": "'%1' er skrivebeskyttet.\nVil du slette den likevel?",
        "Are you sure you want to delete '%1'?": "Er du sikker på at du vil slette '%1'?",
        "Could not delete directory.": "Kunne ikke slette mappen.",
        "Delete": "Slett",
    },
    "ro": {
        "&Choose": "&Alege",
        "&Delete": "&Șterge",
        "&New Folder": "&Folder nou",
        "&No": "&Nu",
        "&Open": "&Deschide",
        "&Rename": "&Redenumire",
        "&Save": "&Salvează",
        "&Yes": "&Da",
        "Abort": "Întrerupe",
        "All Files (*)": "Toate fișierele (*)",
        "All files (*)": "Toate fișierele (*)",
        "Apply": "Aplică",
        "Back": "Înapoi",
        "Cancel": "Anulează",
        "Change to detail view mode": "Schimbă la modul detalii",
        "Change to list view mode": "Schimbă la modul listă",
        "Close": "Închide",
        "Close without Saving": "Închide fără salvare",
        "Create New Folder": "Creează folder nou",
        "Create a New Folder": "Creează un folder nou",
        "Detail View": "Vizualizare detalii",
        "Directories": "Directoare",
        "Directory:": "Director:",
        "Discard": "Renunță",
        "Don't Save": "Nu salva",
        "File &name:": "Nume &fișier:",
        "Files": "Fișiere",
        "Files of type:": "Fișiere de tip:",
        "Find Directory": "Găsește director",
        "Forward": "Înainte",
        "Go back": "Mergi înapoi",
        "Go forward": "Mergi înainte",
        "Go to the parent directory": "Mergi la directorul părinte",
        "Help": "Ajutor",
        "Ignore": "Ignoră",
        "List View": "Vizualizare listă",
        "List of places and bookmarks": "Listă de locuri și semne de carte",
        "Look in:": "Caută în:",
        "N&o to All": "N&u pentru toate",
        "New Folder": "Folder nou",
        "OK": "OK",
        "Open": "Deschide",
        "Parent Directory": "Director părinte",
        "Reset": "Resetează",
        "Recent Places": "Locuri recente",
        "Remove": "Elimină",
        "Restore Defaults": "Restabilește valorile implicite",
        "Retry": "Încearcă din nou",
        "Save": "Salvează",
        "Save All": "Salvează tot",
        "Save As": "Salvează ca",
        "Show ": "Arată ",
        "Show &hidden files": "Arată fișierele &ascunse",
        "Sidebar": "Bară laterală",
        "Yes to &All": "Da pentru &toate",
        "%1\nDirectory not found.\nPlease verify the correct directory name was given.": "%1\nDirectorul nu a fost găsit.\nVerificați dacă a fost introdus numele corect al directorului.",
        "%1 already exists.\nDo you want to replace it?": "%1 există deja.\nDoriți să-l înlocuiți?",
        "%1\nFile not found.\nPlease verify the correct file name was given.": "%1\nFișierul nu a fost găsit.\nVerificați dacă a fost introdus numele corect al fișierului.",
        "'%1' is write protected.\nDo you want to delete it anyway?": "'%1' este protejat la scriere.\nDoriți să-l ștergeți oricum?",
        "Are you sure you want to delete '%1'?": "Sigur doriți să ștergeți '%1'?",
        "Could not delete directory.": "Directorul nu a putut fi șters.",
        "Delete": "Șterge",
    },
    "th": {
        "&Choose": "&เลือก",
        "&Delete": "&ลบ",
        "&New Folder": "โฟลเดอร์&ใหม่",
        "&No": "&ไม่",
        "&Open": "&เปิด",
        "&Rename": "เปลี่ยน&ชื่อ",
        "&Save": "&บันทึก",
        "&Yes": "&ใช่",
        "Abort": "ยกเลิกการทำงาน",
        "All Files (*)": "ไฟล์ทั้งหมด (*)",
        "All files (*)": "ไฟล์ทั้งหมด (*)",
        "Apply": "นำไปใช้",
        "Back": "ย้อนกลับ",
        "Cancel": "ยกเลิก",
        "Change to detail view mode": "เปลี่ยนเป็นมุมมองแบบรายละเอียด",
        "Change to list view mode": "เปลี่ยนเป็นมุมมองแบบรายการ",
        "Close": "ปิด",
        "Close without Saving": "ปิดโดยไม่บันทึก",
        "Create New Folder": "สร้างโฟลเดอร์ใหม่",
        "Create a New Folder": "สร้างโฟลเดอร์ใหม่",
        "Detail View": "มุมมองแบบรายละเอียด",
        "Directories": "ไดเรกทอรี",
        "Directory:": "ไดเรกทอรี:",
        "Discard": "ละทิ้ง",
        "Don't Save": "ไม่บันทึก",
        "File &name:": "ชื่อ&ไฟล์:",
        "Files": "ไฟล์",
        "Files of type:": "ชนิดไฟล์:",
        "Find Directory": "ค้นหาไดเรกทอรี",
        "Forward": "ไปข้างหน้า",
        "Go back": "ย้อนกลับ",
        "Go forward": "ไปข้างหน้า",
        "Go to the parent directory": "ไปยังไดเรกทอรีแม่",
        "Help": "ช่วยเหลือ",
        "Ignore": "ละเว้น",
        "List View": "มุมมองแบบรายการ",
        "List of places and bookmarks": "รายการสถานที่และที่คั่นหน้า",
        "Look in:": "ดูใน:",
        "N&o to All": "ไม่ทั้งหมด",
        "New Folder": "โฟลเดอร์ใหม่",
        "OK": "ตกลง",
        "Open": "เปิด",
        "Parent Directory": "ไดเรกทอรีแม่",
        "Reset": "รีเซ็ต",
        "Recent Places": "สถานที่ล่าสุด",
        "Remove": "เอาออก",
        "Restore Defaults": "คืนค่าเริ่มต้น",
        "Retry": "ลองใหม่",
        "Save": "บันทึก",
        "Save All": "บันทึกทั้งหมด",
        "Save As": "บันทึกเป็น",
        "Show ": "แสดง ",
        "Show &hidden files": "แสดงไฟล์ที่ซ่อนอยู่",
        "Sidebar": "แถบด้านข้าง",
        "Yes to &All": "ใช่ทั้งหมด",
        "%1\nDirectory not found.\nPlease verify the correct directory name was given.": "%1\nไม่พบไดเรกทอรี\nโปรดตรวจสอบว่าระบุชื่อไดเรกทอรีถูกต้อง",
        "%1 already exists.\nDo you want to replace it?": "%1 มีอยู่แล้ว\nคุณต้องการแทนที่หรือไม่?",
        "%1\nFile not found.\nPlease verify the correct file name was given.": "%1\nไม่พบไฟล์\nโปรดตรวจสอบว่าระบุชื่อไฟล์ถูกต้อง",
        "'%1' is write protected.\nDo you want to delete it anyway?": "'%1' มีการป้องกันการเขียน\nคุณต้องการลบอยู่ดีหรือไม่?",
        "Are you sure you want to delete '%1'?": "คุณแน่ใจหรือไม่ว่าต้องการลบ '%1'?",
        "Could not delete directory.": "ไม่สามารถลบไดเรกทอรีได้",
        "Delete": "ลบ",
    },
}


def sanitize_ascii(text: str) -> str:
    return text.encode("ascii", "xmlcharrefreplace").decode("ascii")


def text_of(element: ET.Element | None) -> str:
    if element is None:
        return ""
    text = "".join(element.itertext())
    return text.strip("\n")


def parse_catalog(path: Path) -> tuple[str, dict[str, dict[str, str]], dict[str, str]]:
    tree = ET.parse(path)
    root = tree.getroot()
    language = root.attrib.get("language", "")
    contexts: dict[str, dict[str, str]] = {}
    fallback: dict[str, str] = {}
    for context in root.findall("context"):
        context_name = text_of(context.find("name"))
        messages: dict[str, str] = {}
        for message in context.findall("message"):
            source = text_of(message.find("source"))
            translation = text_of(message.find("translation"))
            if not source or not translation:
                continue
            messages[source] = translation
            fallback.setdefault(source, translation)
        if messages:
            contexts[context_name] = messages
    return language, contexts, fallback


def build_qt_sources(code: str) -> tuple[dict[str, dict[str, str]], dict[str, str]]:
    context_messages: dict[str, dict[str, str]] = {}
    fallback_messages: dict[str, str] = {}
    for qt_code in QT_CODE_MAP.get(code, []):
        for stem in (f"qtbase_{qt_code}.ts", f"qt_{qt_code}.ts"):
            path = QT_TRANSLATIONS_DIR / stem
            if not path.exists():
                continue
            _, contexts, fallback = parse_catalog(path)
            for context_name, messages in contexts.items():
                context_messages.setdefault(context_name, {}).update(messages)
            fallback_messages.update(fallback)
    return context_messages, fallback_messages


def load_template() -> list[tuple[str, list[str]]]:
    tree = ET.parse(TEMPLATE_FILE)
    root = tree.getroot()
    template: list[tuple[str, list[str]]] = []
    for context in root.findall("context"):
        context_name = text_of(context.find("name"))
        if context_name not in TEMPLATE_CONTEXTS:
            continue
        sources: list[str] = []
        for message in context.findall("message"):
            source = text_of(message.find("source"))
            if source:
                sources.append(source)
        template.append((context_name, sources))
    return template


def lookup_translation(
    code: str,
    context_name: str,
    source: str,
    qt_contexts: dict[str, dict[str, str]],
    qt_fallback: dict[str, str],
    oscar_fallback: dict[str, str],
) -> str:
    if source in qt_contexts.get(context_name, {}):
        return qt_contexts[context_name][source]
    if source in qt_fallback:
        return qt_fallback[source]
    if source in MANUAL_OVERRIDES.get(code, {}):
        return MANUAL_OVERRIDES[code][source]
    if source in oscar_fallback:
        return oscar_fallback[source]
    return source


def write_ts_file(
    path: Path,
    language: str,
    template: list[tuple[str, list[str]]],
    code: str,
    qt_contexts: dict[str, dict[str, str]],
    qt_fallback: dict[str, str],
    oscar_fallback: dict[str, str],
) -> None:
    ts = ET.Element("TS", version="2.1", language=language, sourcelanguage="en_US")
    for context_name, sources in template:
        context = ET.SubElement(ts, "context")
        name = ET.SubElement(context, "name")
        name.text = context_name
        for source in sources:
            message = ET.SubElement(context, "message")
            source_element = ET.SubElement(message, "source")
            source_element.text = source
            translation = ET.SubElement(message, "translation")
            translation.text = lookup_translation(
                code, context_name, source, qt_contexts, qt_fallback, oscar_fallback
            )
    ET.indent(ts, space="    ")
    xml = ET.tostring(ts, encoding="unicode")
    output = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!DOCTYPE TS>\n"
        f"{xml}\n"
    )
    path.write_text(sanitize_ascii(output), encoding="utf-8")


def extract_code(filename: str) -> str:
    match = re.search(r"\.([^.]+)\.ts$", filename)
    if not match:
        raise ValueError(f"Could not determine language code from {filename}")
    return match.group(1)


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    template = load_template()

    for translation_path in sorted(TRANSLATIONS_DIR.glob("*.ts")):
        code = extract_code(translation_path.name)
        language, _, oscar_fallback = parse_catalog(translation_path)
        qt_contexts, qt_fallback = build_qt_sources(code)
        output_path = OUTPUT_DIR / f"oscar_qt_{code}.ts"
        write_ts_file(
            output_path, language, template, code, qt_contexts, qt_fallback, oscar_fallback
        )
        print(f"Generated {output_path.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()

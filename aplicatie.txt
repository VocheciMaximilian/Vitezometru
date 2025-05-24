import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
import serial
import serial.tools.list_ports
import threading
import time
import os
import configparser
import csv
from io import StringIO # Folosit pentru a trata un string ca pe un fișier CSV în memorie
import sqlite3
from datetime import datetime

# --- CONSTANTE GLOBALE ---
CMD_REQUEST_TRIPS = "EXPORT_TRIPS_NOW\n"  # Comanda trimisă Arduino pentru a solicita exportul turelor
CONFIG_FILE = "app_config.ini"        # Numele fișierului de configurare al aplicației
DB_FILE = "bike_trips.db"             # Numele fișierului bazei de date SQLite

class ArduinoTripsApp:
    """
    Clasa principală pentru aplicația de gestionare a turelor de bicicletă
    înregistrate de un dispozitiv Arduino.
    Permite conectarea la Arduino, importul turelor (automat sau manual din CSV),
    stocarea lor într-o bază de date SQLite, afișarea, filtrarea și exportul datelor.
    """

    def delete_selected_trip_from_db(self):
        """
        Șterge una sau mai multe ture selectate din Treeview din baza de date.
        Afișează un dialog de confirmare înainte de ștergere.
        """
        selected_items_tree_ids = self.tree.selection() # Obține ID-urile din Treeview (care sunt ID-urile din BD)
        if not selected_items_tree_ids:
            messagebox.showwarning("Atenție", "Nicio tură selectată pentru ștergere.")
            return

        if not messagebox.askyesno("Confirmare Ștergere",
                                   f"Sigur doriți să ștergeți permanent {len(selected_items_tree_ids)} tură(i) selectată(e) din baza de date?"):
            return

        # ID-urile din Treeview sunt direct ID-urile din baza de date (setate la inserare)
        ids_to_delete_from_db = list(selected_items_tree_ids)

        if not ids_to_delete_from_db: return

        try:
            # Construiește query-ul SQL pentru ștergerea mai multor ID-uri
            placeholders = ','.join('?' for _ in ids_to_delete_from_db)
            query = f"DELETE FROM trips WHERE id IN ({placeholders})"
            self.db_cursor.execute(query, ids_to_delete_from_db)
            self.db_conn.commit()
            self.arduino_log_message(f"{len(ids_to_delete_from_db)} tură(i) ștersă(e) din baza de date.")
            self.refresh_table_from_db()  # Reîmprospătează tabelul pentru a reflecta ștergerea
            self.delete_trip_button.config(state=tk.DISABLED) # Dezactivează butonul de ștergere după operație
        except sqlite3.Error as e:
            self.arduino_log_message(f"Eroare la ștergerea turelor din BD: {e}")
            messagebox.showerror("Eroare BD", f"Nu s-au putut șterge turele:\n{e}")

    def __init__(self, root):
        """
        Inițializează interfața grafică și variabilele necesare.
        :param root: Fereastra principală Tkinter.
        """
        self.root = root
        self.root.title("Arduino Bike Trips DB Manager")
        self.root.geometry("1050x800") # Dimensiunea inițială a ferestrei

        # Variabile de stare și conexiuni
        self.serial_connection = None   # Obiectul conexiunii seriale
        self.is_connected = False       # Flag pentru starea conexiunii seriale
        self.is_reading_data = False    # Flag pentru a controla thread-ul de citire de pe serială
        self.working_directory = tk.StringVar(value=os.getcwd()) # Directorul de lucru curent
        self.current_data_in_table_for_export = [] # Listă pentru a stoca datele afișate curent în tabel

        # Conexiunea la baza de date
        self.db_conn = None
        self.db_cursor = None

        # --- CONFIGURARE INTERFAȚĂ GRAFICĂ (GUI) ---

        # Frame pentru setările directorului de lucru
        settings_frame = ttk.LabelFrame(root, text="Setări Director de Lucru (pentru import/export CSV)")
        settings_frame.pack(padx=10, pady=5, fill="x")
        ttk.Label(settings_frame, text="Director:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self.dir_entry = ttk.Entry(settings_frame, textvariable=self.working_directory, width=80, state="readonly")
        self.dir_entry.grid(row=0, column=1, padx=5, pady=5, sticky="ew")
        ttk.Button(settings_frame, text="Selectează...", command=self.select_working_directory).grid(row=0, column=2, padx=5, pady=5)
        settings_frame.grid_columnconfigure(1, weight=1) # Permite câmpului de director să se extindă

        # Frame pentru conexiunea serială
        serial_frame = ttk.LabelFrame(root, text="Conexiune Serială cu Arduino")
        serial_frame.pack(padx=10, pady=5, fill="x")
        ttk.Label(serial_frame, text="Port COM:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self.port_var = tk.StringVar()
        self.port_combobox = ttk.Combobox(serial_frame, textvariable=self.port_var, width=15, state="readonly")
        self.port_combobox.grid(row=0, column=1, padx=5, pady=5)
        self.connect_button = ttk.Button(serial_frame, text="Conectare", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=2, padx=5, pady=5)
        self.refresh_ports_button = ttk.Button(serial_frame, text="Reîmprospătare Porturi", command=self.populate_ports)
        self.refresh_ports_button.grid(row=0, column=3, padx=5, pady=5)
        self.request_data_button = ttk.Button(serial_frame, text="Importă Ture Noi de la Arduino", command=self.request_trips_data, state=tk.DISABLED)
        self.request_data_button.grid(row=0, column=4, padx=10, pady=5)

        # Frame pentru import manual CSV
        csv_import_frame = ttk.LabelFrame(root, text="Import Manual CSV în Baza de Date")
        csv_import_frame.pack(padx=10, pady=5, fill="x")
        ttk.Label(csv_import_frame, text="Fișiere CSV în director:").pack(side=tk.LEFT, padx=10, pady=5)
        self.csv_files_var = tk.StringVar()
        self.csv_files_combobox = ttk.Combobox(csv_import_frame, textvariable=self.csv_files_var, width=40, state="readonly")
        self.csv_files_combobox.pack(side=tk.LEFT, padx=5, pady=5)
        self.csv_files_combobox.bind("<<ComboboxSelected>>", self.on_csv_file_selected_for_import)
        self.import_csv_button = ttk.Button(csv_import_frame, text="Importă CSV Selectat în BD", command=self.import_selected_csv_file_to_db, state=tk.DISABLED)
        self.import_csv_button.pack(side=tk.LEFT, padx=5, pady=5)

        # Frame pentru filtrarea datelor afișate
        filter_frame = ttk.LabelFrame(root, text="Filtrare Ture Afișate")
        filter_frame.pack(padx=10, pady=5, fill="x")
        ttk.Label(filter_frame, text="Dată (YYYY-MM-DD):").grid(row=0, column=0, padx=5, pady=2, sticky="w")
        self.date_filter_entry = ttk.Entry(filter_frame, width=12)
        self.date_filter_entry.grid(row=0, column=1, padx=5, pady=2)
        ttk.Label(filter_frame, text="Luna (YYYY-MM):").grid(row=0, column=2, padx=5, pady=2, sticky="w")
        self.month_filter_entry = ttk.Entry(filter_frame, width=9)
        self.month_filter_entry.grid(row=0, column=3, padx=5, pady=2)
        ttk.Label(filter_frame, text="Dist. Min (km):").grid(row=0, column=4, padx=5, pady=2, sticky="w")
        self.dist_min_filter_entry = ttk.Entry(filter_frame, width=7)
        self.dist_min_filter_entry.grid(row=0, column=5, padx=5, pady=2)
        ttk.Button(filter_frame, text="Aplică Filtre", command=self.apply_filters_and_refresh_table).grid(row=0, column=6, padx=10, pady=2)
        ttk.Button(filter_frame, text="Resetează Filtre", command=self.reset_filters_and_refresh_table).grid(row=0, column=7, padx=5, pady=2)

        # Frame pentru tabelul de ture (Treeview)
        table_frame = ttk.LabelFrame(root, text="Ture din Baza de Date")
        table_frame.pack(padx=10, pady=10, fill="both", expand=True)

        # Definirea coloanelor pentru Treeview și maparea lor la coloanele din BD
        self.tree_columns_db_map = {
            "ID BD": "id", "V.Med(km/h)": "avg_speed", "V.Max(km/h)": "max_speed",
            "V.Min(km/h)": "min_speed", "Distanță(km)": "distance_km",
            "Durată(s)": "duration_s", "Start Tură": "timestamp_iso"
        }
        self.tree_display_columns = tuple(self.tree_columns_db_map.keys()) # Numele coloanelor așa cum apar în GUI
        self.tree = ttk.Treeview(table_frame, columns=self.tree_display_columns, show="headings", height=15)

        # Configurare antete și coloane Treeview
        for display_col_text in self.tree_display_columns:
            db_col_name = self.tree_columns_db_map[display_col_text]
            # Setează comanda de sortare pentru fiecare antet de coloană
            self.tree.heading(display_col_text, text=display_col_text,
                              command=lambda _db_col=db_col_name: self.sort_treeview_column_from_db(_db_col, False))
            # Setează lățimea și alinierea specifică pentru fiecare coloană
            if display_col_text == "ID BD": self.tree.column(display_col_text, width=50, anchor='center', stretch=tk.NO)
            elif "V." in display_col_text: self.tree.column(display_col_text, width=100, anchor='e') # 'e' = east (dreapta)
            elif display_col_text == "Distanță(km)": self.tree.column(display_col_text, width=100, anchor='e')
            elif display_col_text == "Durată(s)": self.tree.column(display_col_text, width=80, anchor='e')
            elif display_col_text == "Start Tură": self.tree.column(display_col_text, width=160, anchor='w') # 'w' = west (stânga)
            else: self.tree.column(display_col_text, width=90, anchor='center')

        # Scrollbar-uri pentru Treeview
        vsb = ttk.Scrollbar(table_frame, orient="vertical", command=self.tree.yview)
        hsb = ttk.Scrollbar(table_frame, orient="horizontal", command=self.tree.xview)
        self.tree.configure(yscrollcommand=vsb.set, xscrollcommand=hsb.set)
        vsb.pack(side='right', fill='y')
        hsb.pack(side='bottom', fill='x')
        self.tree.pack(fill="both", expand=True) # Face Treeview să umple spațiul disponibil

        # Frame pentru butoanele de sub tabel
        table_buttons_frame = ttk.Frame(table_frame)
        table_buttons_frame.pack(fill='x', pady=5)
        self.clear_table_button = ttk.Button(table_buttons_frame, text="Curăță Afișaj Tabel", command=self.clear_table_display_only)
        self.clear_table_button.pack(side=tk.LEFT, padx=5)
        self.export_visible_to_csv_button = ttk.Button(table_buttons_frame, text="Exportă Datele Vizibile ca CSV", command=self.export_visible_table_to_csv, state=tk.DISABLED)
        self.export_visible_to_csv_button.pack(side=tk.LEFT, padx=5)
        ttk.Button(table_buttons_frame, text="Vezi Top Rezultate", command=self.show_top_results_window).pack(side=tk.LEFT, padx=10)
        self.delete_trip_button = ttk.Button(table_buttons_frame, text="Șterge Tura Selectată din BD", command=self.delete_selected_trip_from_db, state=tk.DISABLED)
        self.delete_trip_button.pack(side=tk.RIGHT, padx=5)
        self.tree.bind("<<TreeviewSelect>>", self.on_tree_item_select) # Activează butonul de ștergere la selecție

        # Frame pentru log-ul aplicației/Arduino
        self._log_frame_arduino_container = ttk.LabelFrame(root, text="Log Mesaje Aplicație/Arduino")
        self._log_frame_arduino_container.pack(padx=10, pady=5, fill="x", expand=False)
        self.arduino_log_text = tk.Text(self._log_frame_arduino_container, wrap=tk.WORD, width=80, height=4, state='disabled')
        self.arduino_log_text.pack(fill="x", expand=True, padx=5, pady=5)

        # --- INIȚIALIZĂRI FINALE ---
        self.setup_database()        # Configurează sau se conectează la baza de date
        self.load_config()           # Încarcă configurația (directorul de lucru)
        self.populate_ports()        # Populează lista de porturi COM disponibile
        self.refresh_table_from_db() # Afișează datele inițiale din BD în tabel

        # Gestionează închiderea ferestrei
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)


    def on_tree_item_select(self, event):
        """
        Callback apelat la selectarea unui element în Treeview.
        Activează sau dezactivează butonul de ștergere în funcție de selecție.
        """
        selected_items = self.tree.selection()
        if selected_items:
            self.delete_trip_button.config(state=tk.NORMAL)
        else:
            self.delete_trip_button.config(state=tk.DISABLED)

    def arduino_log_message(self, message):
        """
        Adaugă un mesaj în widget-ul de log al aplicației.
        :param message: String-ul mesajului de afișat.
        """
        if hasattr(self, 'arduino_log_text') and self.arduino_log_text: # Verifică dacă widget-ul există
            self.arduino_log_text.configure(state='normal') # Permite modificarea textului
            self.arduino_log_text.insert(tk.END, message + "\n")
            self.arduino_log_text.configure(state='disabled') # Blochează modificarea manuală
            self.arduino_log_text.see(tk.END) # Rulează automat la ultimul mesaj
        else:
            print(f"LOG (widget indisponibil): {message}") # Fallback la consolă

    def setup_database(self):
        """
        Creează (dacă nu există) și se conectează la baza de date SQLite.
        Definește structura tabelului 'trips'.
        """
        try:
            self.db_conn = sqlite3.connect(DB_FILE)
            self.db_cursor = self.db_conn.cursor()
            # Definirea tabelului 'trips' cu coloanele necesare
            # `timestamp_iso` este UNIC pentru a preveni duplicatele exacte
            self.db_cursor.execute('''
                CREATE TABLE IF NOT EXISTS trips (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    trip_id_original INTEGER, 
                    avg_speed REAL, max_speed REAL, min_speed REAL, 
                    distance_km REAL, duration_s INTEGER, 
                    start_year INTEGER, start_month INTEGER, start_day INTEGER, 
                    start_hour INTEGER, start_minute INTEGER, start_second INTEGER, 
                    timestamp_iso TEXT UNIQUE
                )
            ''')
            self.db_conn.commit()
            self.arduino_log_message(f"Baza de date '{DB_FILE}' conectată/creată.")
        except sqlite3.Error as e:
            messagebox.showerror("Eroare Bază de Date", f"Eroare BD: {e}")
            if self.root: self.root.destroy() # Închide aplicația în caz de eroare critică BD
        except Exception as ex: # Captură pentru alte erori neașteptate la inițializare BD
            print(f"EROARE CRITICĂ în setup_database: {ex}")
            if self.root: self.root.destroy()

    def insert_trip_to_db(self, trip_data_list_of_lists, from_arduino=False):
        """
        Inserează o listă de ture (fiecare tură fiind o listă de valori) în baza de date.
        Convertește datele la tipurile corecte și gestionează duplicatele (prin `INSERT OR IGNORE`).
        :param trip_data_list_of_lists: O listă de liste, unde fiecare listă interioară reprezintă o tură.
        :param from_arduino: Flag pentru a indica dacă datele provin direct de la Arduino (pentru logare și refresh).
        :return: Numărul de ture inserate efectiv.
        """
        if not self.db_conn: return 0 # Nu face nimic dacă nu există conexiune la BD

        insert_query = '''
            INSERT OR IGNORE INTO trips (trip_id_original, avg_speed, max_speed, min_speed, 
                               distance_km, duration_s, start_year, start_month, 
                               start_day, start_hour, start_minute, start_second, timestamp_iso)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) 
        '''
        # `INSERT OR IGNORE`: Dacă o constrângere de unicitate (ex. pe `timestamp_iso`) este violată,
        # rândul nu este inserat, dar nu se generează o eroare.

        trips_to_insert_tuples = [] # Listă pentru a stoca tuplele pregătite pentru inserare
        for row_list in trip_data_list_of_lists:
            if len(row_list) == 12: # Verifică numărul corect de coloane (conform CSV-ului Arduino)
                try:
                    # Conversia datelor din string la tipurile numerice/datetime corespunzătoare
                    trip_id_orig = int(row_list[0]) if row_list[0] else None # ID-ul original de la Arduino (poate fi null)
                    avg_s = float(row_list[1])
                    max_s = float(row_list[2])
                    min_s = float(row_list[3])
                    dist = float(row_list[4])
                    dur = int(row_list[5])
                    year, month, day = int(row_list[6]), int(row_list[7]), int(row_list[8])
                    hour, minute, second = int(row_list[9]), int(row_list[10]), int(row_list[11])

                    # Crearea unui obiect datetime și formatarea lui ca string ISO pentru stocare și unicitate
                    dt_obj = datetime(year, month, day, hour, minute, second)
                    timestamp_iso_str = dt_obj.strftime('%Y-%m-%d %H:%M:%S')

                    trips_to_insert_tuples.append((
                        trip_id_orig, avg_s, max_s, min_s, dist, dur,
                        year, month, day, hour, minute, second, timestamp_iso_str
                    ))
                except ValueError as ve:
                    self.arduino_log_message(f"Eroare conversie date pt rândul {row_list}: {ve}")
                    continue # Trece la următorul rând dacă datele sunt invalide
            else:
                self.arduino_log_message(f"Rând CSV ignorat pt BD (nr. coloane incorect): {row_list}")

        if trips_to_insert_tuples:
            try:
                # Numără rândurile înainte de inserare pentru a determina câte au fost adăugate efectiv
                self.db_cursor.execute("SELECT COUNT(*) FROM trips")
                count_before = self.db_cursor.fetchone()[0]

                self.db_cursor.executemany(insert_query, trips_to_insert_tuples) # Inserează toate turele valide
                self.db_conn.commit()

                self.db_cursor.execute("SELECT COUNT(*) FROM trips")
                count_after = self.db_cursor.fetchone()[0]

                inserted_this_time = count_after - count_before
                total_processed = len(trips_to_insert_tuples)
                ignored_count = total_processed - inserted_this_time # Ture care nu au fost inserate (probabil duplicate)

                log_msg = f"{inserted_this_time} ture noi adăugate."
                if ignored_count > 0:
                    log_msg += f" {ignored_count} ture ignorate (probabil duplicate)."
                self.arduino_log_message(log_msg)

                # Dacă datele provin de la Arduino sau dacă s-au inserat date noi și utilizatorul confirmă, reîmprospătează tabelul
                if from_arduino or (inserted_this_time > 0 and messagebox.askyesno("Reîmprospătare Tabel", "Datele au fost adăugate în baza de date. Doriți să reîmprospătați tabelul acum?")):
                    self.refresh_table_from_db()
                return inserted_this_time
            except sqlite3.Error as e:
                self.arduino_log_message(f"Eroare BD la inserare: {e}")
        return 0 # Returnează 0 dacă nu s-a inserat nimic


    def refresh_table_from_db(self, query=None, params=()):
        """
        Reîncarcă datele din baza de date în Treeview.
        Poate primi un query specific (ex. pentru sortare sau filtrare).
        :param query: Query-ul SQL de executat. Dacă None, se folosește un query default.
        :param params: Parametrii pentru query-ul SQL.
        """
        if query is None:
            # Construiește query-ul default pentru a selecta coloanele în ordinea definită în GUI
            query_select_cols = [self.tree_columns_db_map[col_text] for col_text in self.tree_display_columns]
            query = f"SELECT {', '.join(query_select_cols)} FROM trips ORDER BY timestamp_iso DESC" # Sortează descrescător după timestamp by default

        if not self.db_conn: return
        self.clear_table_display_only(reset_internal_data=True) # Golește tabelul înainte de a-l repopula

        try:
            self.db_cursor.execute(query, params)
            rows_from_db = self.db_cursor.fetchall() # Obține toate rândurile rezultate

            if not rows_from_db:
                self.arduino_log_message("Nicio tură în BD pentru afișare (conform filtrelor).")
                return

            for db_row_tuple in rows_from_db:
                if len(db_row_tuple) == len(self.tree_display_columns):
                    # Inserează rândul în Treeview. `iid` este ID-ul din BD pentru a facilita ștergerea.
                    self.tree.insert("", tk.END, values=db_row_tuple, iid=str(db_row_tuple[0])) # db_row_tuple[0] este 'id'
                    self.current_data_in_table_for_export.append(list(db_row_tuple)) # Stochează datele pentru export CSV
                else:
                     self.arduino_log_message(f"Date BD incompatibile cu tabelul (nr. coloane): {db_row_tuple}")

            # Activează butonul de export dacă există date în tabel
            if self.current_data_in_table_for_export:
                self.export_visible_to_csv_button.config(state=tk.NORMAL)

        except sqlite3.Error as e:
            self.arduino_log_message(f"Eroare citire date din BD: {e}")


    def sort_treeview_column_from_db(self, db_column_name_to_sort_by, reverse):
        """
        Sortează datele afișate în Treeview pe baza unei coloane specifice din BD.
        Păstrează filtrele curente aplicate.
        :param db_column_name_to_sort_by: Numele coloanei din BD după care se sortează.
        :param reverse: Boolean, True pentru sortare descrescătoare, False pentru crescătoare.
        """
        conditions, params = self.get_current_filters_sql() # Obține filtrele curente
        if conditions is None: return # Eroare la obținerea filtrelor

        # Obține lista coloanelor de selectat în ordinea GUI
        query_select_cols = [self.tree_columns_db_map[col_text] for col_text in self.tree_display_columns]
        select_cols_str = ", ".join(query_select_cols)
        base_query = f"SELECT {select_cols_str} FROM trips"

        order_clause = f"ORDER BY {db_column_name_to_sort_by} {'DESC' if reverse else 'ASC'}"

        # Construiește query-ul final incluzând filtrele și clauza de sortare
        if conditions:
            query = f"{base_query} WHERE {' AND '.join(conditions)} {order_clause}"
        else:
            query = f"{base_query} {order_clause}"

        self.refresh_table_from_db(query, tuple(params)) # Reîmprospătează tabelul cu datele sortate

        # Actualizează comanda antetului coloanei pentru a inversa sortarea la următorul click
        gui_col_text_to_update = None
        for gui_text, db_col in self.tree_columns_db_map.items():
            if db_col == db_column_name_to_sort_by:
                gui_col_text_to_update = gui_text
                break

        if gui_col_text_to_update:
            self.tree.heading(gui_col_text_to_update,
                              command=lambda _db_col=db_column_name_to_sort_by: self.sort_treeview_column_from_db(_db_col, not reverse))
        else: # Fallback: resetează comenzile de sortare pentru toate coloanele (puțin probabil să fie necesar)
            for gui_text_reset, db_col_reset in self.tree_columns_db_map.items():
                 self.tree.heading(gui_text_reset,
                                   command=lambda _db_col=db_col_reset: self.sort_treeview_column_from_db(_db_col, False))


    def clear_table_display_only(self, reset_internal_data=False):
        """
        Golește toate rândurile din Treeview.
        :param reset_internal_data: Dacă True, resetează și lista internă de date pentru export.
        """
        for item in self.tree.get_children():
            self.tree.delete(item)
        if reset_internal_data:
            self.current_data_in_table_for_export = []
            self.export_visible_to_csv_button.config(state=tk.DISABLED)


    def get_current_filters_sql(self):
        """
        Construiește clauzele WHERE și parametrii pentru query-ul SQL pe baza filtrelor introduse în GUI.
        Validează formatele datelor introduse.
        :return: Tuplu (listă de condiții SQL, listă de parametri) sau (None, None) în caz de eroare de validare.
        """
        conditions = []
        params = []
        date_str = self.date_filter_entry.get().strip()
        if date_str:
            try:
                datetime.strptime(date_str, '%Y-%m-%d') # Validează formatul datei
                conditions.append("DATE(timestamp_iso) = ?") # SQLite DATE() function
                params.append(date_str)
            except ValueError:
                messagebox.showerror("Format Dată Invalid", "Formatul corect pentru dată este: YYYY-MM-DD.")
                return None, None

        month_str = self.month_filter_entry.get().strip()
        if month_str and not date_str: # Filtrul de lună se aplică doar dacă nu e specificată o dată exactă
            try:
                datetime.strptime(month_str, '%Y-%m') # Validează formatul lunii
                conditions.append("STRFTIME('%Y-%m', timestamp_iso) = ?") # SQLite STRFTIME()
                params.append(month_str)
            except ValueError:
                messagebox.showerror("Format Lună Invalid", "Formatul corect pentru lună este: YYYY-MM.")
                return None, None

        dist_min_str = self.dist_min_filter_entry.get().strip()
        if dist_min_str:
            try:
                dist_min = float(dist_min_str) # Validează că distanța este un număr
                conditions.append("distance_km >= ?")
                params.append(dist_min)
            except ValueError:
                messagebox.showerror("Format Distanță Invalid", "Distanța minimă trebuie să fie un număr (ex: 0.5).")
                return None, None
        return conditions, params


    def apply_filters_and_refresh_table(self):
        """
        Aplică filtrele curente și reîmprospătează tabelul.
        """
        conditions, params = self.get_current_filters_sql()
        if conditions is None: return # Nu continuă dacă filtrele sunt invalide

        query_select_cols = [self.tree_columns_db_map[col_text] for col_text in self.tree_display_columns]
        select_cols_str = ", ".join(query_select_cols)
        base_query = f"SELECT {select_cols_str} FROM trips"

        # Construiește query-ul final cu filtrele
        if conditions:
            query = f"{base_query} WHERE {' AND '.join(conditions)} ORDER BY timestamp_iso DESC"
        else: # Niciun filtru aplicat
            query = f"{base_query} ORDER BY timestamp_iso DESC"

        self.refresh_table_from_db(query, tuple(params))


    def reset_filters_and_refresh_table(self):
        """
        Resetează câmpurile de filtrare și reîncarcă toate datele în tabel.
        """
        self.date_filter_entry.delete(0, tk.END)
        self.month_filter_entry.delete(0, tk.END)
        self.dist_min_filter_entry.delete(0, tk.END)
        self.refresh_table_from_db() # Reîncarcă fără filtre

    def on_csv_file_selected_for_import(self, event=None):
        """
        Callback apelat la selectarea unui fișier CSV din Combobox.
        Activează butonul de import dacă un fișier valid este selectat.
        """
        selected_file = self.csv_files_var.get()
        # Verifică dacă fișierul selectat nu este un placeholder de eroare/lipsă
        if selected_file and selected_file not in ["Niciun fișier CSV", "Eroare director", "Director invalid"]:
            self.import_csv_button.config(state=tk.NORMAL)
        else:
            self.import_csv_button.config(state=tk.DISABLED)


    def import_selected_csv_file_to_db(self):
        """
        Importă datele din fișierul CSV selectat în baza de date.
        Validează antetul fișierului CSV.
        """
        selected_file = self.csv_files_var.get()
        if not selected_file or selected_file in ["Niciun fișier CSV", "Eroare director", "Director invalid"]:
            messagebox.showwarning("Atenție", "Niciun fișier CSV valid selectat pentru import.")
            return

        filepath = os.path.join(self.working_directory.get(), selected_file)
        if not os.path.exists(filepath):
            messagebox.showerror("Eroare", f"Fișierul '{selected_file}' nu a fost găsit în directorul de lucru.")
            self.refresh_csv_list() # Reîmprospătează lista de fișiere CSV, s-ar putea ca fișierul să fi fost șters
            return
        try:
            with open(filepath, "r", encoding='utf-8', newline='') as f: # `newline=''` este important pentru CSV
                reader = csv.reader(f)
                header = next(reader, None) # Citește primul rând (antetul)
                expected_header = ["ID_Tura","VitezaMedie","VitezaMax","VitezaMin","Distanta_km","Durata_s","An","Luna","Zi","Ora","Minut","Secunda"]
                # Validează dacă antetul corespunde formatului așteptat de la Arduino
                if not header or list(h.strip() for h in header) != expected_header:
                    messagebox.showerror("Antet CSV Invalid", f"Antetul din fișierul '{selected_file}' nu corespunde formatului așteptat.\nFormat așteptat: {','.join(expected_header)}")
                    return

                data_rows_for_db = list(reader) # Citește restul rândurilor

            if data_rows_for_db:
                self.insert_trip_to_db(data_rows_for_db, from_arduino=False) # Inserează datele în BD
            else:
                self.arduino_log_message(f"Nicio dată de importat din fișierul '{selected_file}' (posibil gol după antet).")

        except Exception as e:
            messagebox.showerror("Eroare Import CSV", f"Nu s-a putut importa fișierul '{selected_file}':\n{e}")

    def load_config(self):
        """
        Încarcă configurația aplicației (directorul de lucru) din fișierul CONFIG_FILE.
        Dacă fișierul nu există, se creează unul cu valori default.
        """
        config = configparser.ConfigParser()
        if os.path.exists(CONFIG_FILE):
            config.read(CONFIG_FILE)
            if 'Settings' in config and 'WorkingDirectory' in config['Settings']:
                self.working_directory.set(config['Settings']['WorkingDirectory'])
                # Verifică dacă directorul salvat încă este valid, altfel resetează la directorul curent
                if not os.path.isdir(self.working_directory.get()):
                    self.arduino_log_message(f"Directorul de lucru salvat '{self.working_directory.get()}' nu este valid. Se resetează la cel curent.")
                    self.working_directory.set(os.getcwd())
            else: # Dacă secțiunea sau cheia lipsește, folosește directorul curent
                 self.working_directory.set(os.getcwd())
            self.refresh_csv_list() # Actualizează lista de fișiere CSV pe baza directorului încărcat/setat
        else:
            self.arduino_log_message(f"Fișierul de configurare '{CONFIG_FILE}' nu a fost găsit. Se creează unul nou.")
            self.save_config() # Salvează configurația default (directorul curent)
            self.refresh_csv_list()


    def save_config(self):
        """
        Salvează configurația curentă (directorul de lucru) în fișierul CONFIG_FILE.
        """
        config = configparser.ConfigParser()
        config['Settings'] = {'WorkingDirectory': self.working_directory.get()}
        try:
            with open(CONFIG_FILE, 'w') as configfile:
                config.write(configfile)
        except Exception as e:
            self.arduino_log_message(f"Eroare la salvarea configurației în '{CONFIG_FILE}': {e}")


    def select_working_directory(self):
        """
        Deschide un dialog pentru selectarea directorului de lucru.
        Actualizează directorul și lista de fișiere CSV, apoi salvează configurația.
        """
        directory = filedialog.askdirectory(initialdir=self.working_directory.get(), title="Selectează Directorul de Lucru")
        if directory: # Dacă utilizatorul a selectat un director
            self.working_directory.set(directory)
            self.refresh_csv_list() # Actualizează lista de fișiere CSV
            self.save_config()      # Salvează noul director în configurație


    def refresh_csv_list(self):
        """
        Actualizează lista de fișiere CSV afișată în Combobox pe baza directorului de lucru curent.
        Dezactivează butonul de import dacă nu sunt fișiere CSV.
        """
        current_dir = self.working_directory.get()
        self.csv_files_combobox.set('') # Golește selecția curentă
        self.import_csv_button.config(state=tk.DISABLED) # Dezactivează butonul de import inițial

        if os.path.isdir(current_dir):
            try:
                files = [f for f in os.listdir(current_dir) if f.lower().endswith('.csv')]
                self.csv_files_combobox['values'] = files # Populează Combobox-ul
                if files:
                    self.csv_files_combobox.config(state="readonly") # Permite doar selecția din listă
                    # self.csv_files_combobox.set(files[0]) # Opțional: selectează primul fișier
                    # self.on_csv_file_selected_for_import() # Opțional: activează butonul dacă e preselectat
                else:
                    self.csv_files_combobox['values'] = ["Niciun fișier CSV"]
                    self.csv_files_combobox.set("Niciun fișier CSV")
                    self.csv_files_combobox.config(state="disabled") # Dezactivează Combobox-ul
            except Exception as e:
                self.arduino_log_message(f"Eroare la listarea fișierelor CSV din '{current_dir}': {e}")
                self.csv_files_combobox['values'] = ["Eroare director"]
                self.csv_files_combobox.set("Eroare director")
                self.csv_files_combobox.config(state="disabled")
        else:
            self.arduino_log_message(f"Directorul de lucru '{current_dir}' este invalid.")
            self.csv_files_combobox['values'] = ["Director invalid"]
            self.csv_files_combobox.set("Director invalid")
            self.csv_files_combobox.config(state="disabled")

    def populate_ports(self):
        """
        Populează Combobox-ul cu porturile COM seriale disponibile.
        """
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combobox['values'] = ports
        if ports:
            self.port_var.set(ports[0]) # Preselectează primul port găsit
        else:
            self.port_var.set("") # Nu sunt porturi, lasă gol


    def toggle_connection(self):
        """
        Gestionează conectarea și deconectarea de la portul serial selectat.
        Pornește/oprește thread-ul de citire a datelor de la Arduino.
        """
        if not self.is_connected:
            # --- Încearcă să se conecteze ---
            selected_port = self.port_var.get()
            if not selected_port:
                messagebox.showerror("Eroare", "Niciun port COM selectat.")
                return
            try:
                self.serial_connection = serial.Serial(selected_port, 9600, timeout=1)
                # Un scurt delay pentru a permite Arduino să se stabilizeze după conectare
                # (mai ales dacă Arduino se resetează la deschiderea conexiunii seriale)
                time.sleep(2)
                self.is_connected = True
                self.connect_button.config(text="Deconectare")
                self.request_data_button.config(state=tk.NORMAL) # Activează butonul de import de la Arduino
                self.port_combobox.config(state=tk.DISABLED)     # Blochează schimbarea portului cât timp e conectat
                self.refresh_ports_button.config(state=tk.DISABLED)
                self.arduino_log_message(f"Conectat la {selected_port}")

                # Pornește un thread separat pentru a citi datele de la Arduino fără a bloca GUI-ul
                self.read_thread = threading.Thread(target=self.read_from_arduino, daemon=True)
                # `daemon=True` permite închiderea aplicației chiar dacă thread-ul încă rulează
                self.read_thread.start()

            except serial.SerialException as e:
                messagebox.showerror("Eroare Conexiune", f"Nu s-a putut conecta la {selected_port}:\n{e}")
                self.serial_connection = None # Asigură-te că este None dacă conexiunea a eșuat
            except Exception as e: # Captură pentru alte erori la conectare
                 messagebox.showerror("Eroare Necunoscută", f"A apărut o eroare la conectare:\n{e}")
                 self.serial_connection = None
        else:
            # --- Încearcă să se deconecteze ---
            self.is_reading_data = False # Semnalează thread-ului de citire să se oprească
            if hasattr(self, 'read_thread') and self.read_thread.is_alive():
                self.read_thread.join(timeout=1.0) # Așteaptă ca thread-ul să se termine (cu timeout)

            if self.serial_connection and self.serial_connection.is_open:
                try:
                    self.serial_connection.close()
                except Exception as e:
                    self.arduino_log_message(f"Eroare la închiderea portului serial: {e}")

            self.is_connected = False
            self.serial_connection = None
            self.connect_button.config(text="Conectare")
            self.request_data_button.config(state=tk.DISABLED)
            self.port_combobox.config(state=tk.NORMAL) # Permite selectarea altui port
            self.refresh_ports_button.config(state=tk.NORMAL)
            self.arduino_log_message("Deconectat.")


    def read_from_arduino(self):
        """
        Rulează într-un thread separat și citește continuu datele de pe portul serial.
        Identifică blocurile de date CSV trimise de Arduino și le procesează.
        Afișează alte mesaje de la Arduino în log.
        """
        self.is_reading_data = True # Flag pentru a menține bucla activă
        in_csv_export = False      # Flag pentru a indica dacă se citește un bloc CSV
        current_csv_block_string = "" # String pentru a acumula liniile CSV

        try:
            while self.is_connected and self.is_reading_data: # Rulează cât timp e conectat și e permisă citirea
                if self.serial_connection and self.serial_connection.in_waiting > 0: # Verifică dacă sunt date de citit
                    try:
                        line = self.serial_connection.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            # Detectează începutul blocului CSV
                            if "--- EXPORT DATE TURE (CSV) ---" in line:
                                in_csv_export = True
                                current_csv_block_string = "" # Resetează bufferul CSV
                                self.arduino_log_message("Primit început de export CSV de la Arduino.")
                                continue # Trece la următoarea linie

                            # Detectează sfârșitul blocului CSV
                            if "--- SFARSIT EXPORT ---" in line:
                                in_csv_export = False
                                if current_csv_block_string: # Dacă s-a acumulat ceva
                                    final_string_to_process = current_csv_block_string
                                    # Programează procesarea CSV în thread-ul principal GUI pentru siguranță
                                    self.root.after(0, lambda s=final_string_to_process: self.process_arduino_csv_data(s))
                                current_csv_block_string = "" # Resetează bufferul
                                self.arduino_log_message("Export CSV de la Arduino finalizat, se procesează...")
                                continue

                            if in_csv_export:
                                # Acumulează liniile din blocul CSV (fără antetul "--- EXPORT...")
                                if line != "ID_Tura,VitezaMedie,VitezaMax,VitezaMin,Distanta_km,Durata_s,An,Luna,Zi,Ora,Minut,Secunda":
                                    current_csv_block_string += line + "\n"
                            else:
                                # Afișează alte mesaje de la Arduino (ex. log-uri, debug)
                                self.arduino_log_message(f"Arduino: {line}")

                    except serial.SerialException: # Eroare specifică portului serial
                        self.arduino_log_message("Eroare citire port serial. Deconectare automată...")
                        self.root.after(0, self.force_disconnect) # Forțează deconectarea din thread-ul GUI
                        break # Iese din bucla while
                    except UnicodeDecodeError as ude:
                        self.arduino_log_message(f"Eroare decodare date de la Arduino: {ude}")
                    except Exception as e: # Altă eroare la procesarea liniei
                        self.arduino_log_message(f"Eroare la procesarea unei linii de la Arduino: {e}")
                time.sleep(0.1) # Scurtă pauză pentru a nu suprasolicita CPU-ul
        except Exception as e: # Eroare majoră în thread (ex. dacă self.serial_connection devine invalid)
            if self.is_connected: # Loghează doar dacă se presupunea că încă e conectat
                 self.arduino_log_message(f"Eroare majoră în thread-ul de citire Arduino: {e}")
        finally:
            self.is_reading_data = False # Asigură că flag-ul e resetat la ieșirea din thread


    def process_arduino_csv_data(self, csv_data_string):
        """
        Procesează un string care conține date CSV (fără delimitatorii "--- EXPORT ---").
        Inserează datele în baza de date.
        :param csv_data_string: String-ul cu datele CSV primite de la Arduino.
        """
        try:
            # Folosește StringIO pentru a trata string-ul ca pe un fișier CSV
            data_io = StringIO(csv_data_string)
            reader = csv.reader(data_io)
            # Antetul este deja verificat și eliminat în read_from_arduino (sau ar trebui să fie)
            # Dar, pentru robustețe, ar fi bine să îl verificăm din nou sau să ne asigurăm
            # că `csv_data_string` conține doar datele, nu și antetul.
            # Presupunând că `csv_data_string` conține doar rândurile de date:
            data_rows_for_db = list(reader)

            # Trebuie să adăugăm din nou antetul așteptat pentru validarea din insert_trip_to_db,
            # dacă acesta nu este inclus în csv_data_string.
            # Alternativ, modificăm insert_trip_to_db să nu necesite antetul.
            # Momentan, presupunem că `data_rows_for_db` este doar lista de valori.

            if data_rows_for_db:
                self.arduino_log_message(f"Procesare {len(data_rows_for_db)} rânduri CSV de la Arduino...")
                # Metoda insert_trip_to_db se așteaptă la o listă de liste, ceea ce `list(reader)` produce.
                # Ea verifică intern numărul de coloane și face conversiile.
                self.insert_trip_to_db(data_rows_for_db, from_arduino=True)
            else:
                self.arduino_log_message("Nicio dată de tură validă primită în blocul CSV de la Arduino.")
        except Exception as e:
            self.arduino_log_message(f"Eroare la procesarea datelor CSV de la Arduino: {e}\nDate primite:\n{csv_data_string[:200]}...") # Afișează o parte din date pentru debug


    def force_disconnect(self):
        """
        Forțează deconectarea (utilizată de obicei când thread-ul de citire detectează o eroare).
        Rulează în thread-ul GUI.
        """
        if self.is_connected:
            self.toggle_connection() # Apelează metoda standard de deconectare


    def request_trips_data(self):
        """
        Trimite comanda CMD_REQUEST_TRIPS către Arduino pentru a solicita exportul datelor.
        """
        if self.is_connected and self.serial_connection:
            try:
                self.serial_connection.write(CMD_REQUEST_TRIPS.encode('utf-8'))
                self.arduino_log_message(f"Comanda '{CMD_REQUEST_TRIPS.strip()}' trimisă către Arduino.")
            except serial.SerialException as e:
                messagebox.showerror("Eroare Trimitere Comandă", f"Nu s-a putut trimite comanda către Arduino:\n{e}")
                self.toggle_connection() # Deconectează dacă trimiterea eșuează
            except Exception as e:
                messagebox.showerror("Eroare Trimitere Necunoscută", f"Eroare la trimiterea comenzii către Arduino:\n{e}")
        else:
            messagebox.showwarning("Atenție", "Nu sunteți conectat la Arduino.")


    def export_visible_table_to_csv(self):
        """
        Exportă datele vizibile curent în Treeview într-un fișier CSV.
        Utilizatorul este întrebat unde să salveze fișierul.
        """
        if not self.current_data_in_table_for_export: # Verifică dacă sunt date de exportat
            messagebox.showwarning("Atenție", "Nu există date în tabel pentru export.")
            return

        header_for_export = self.tree_display_columns # Folosește antetele așa cum sunt afișate în GUI
        csv_content_to_save = ",".join(header_for_export) + "\n" # Creează linia de antet

        # Adaugă fiecare rând de date la conținutul CSV
        for row_data_tuple in self.current_data_in_table_for_export:
            csv_content_to_save += ",".join(str(x) for x in row_data_tuple) + "\n"

        # Generează un nume de fișier default cu timestamp
        initial_filename = f"export_ture_vizibile_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        filepath_in_dialog = filedialog.asksaveasfilename(
            initialdir=self.working_directory.get(),
            initialfile=initial_filename,
            defaultextension=".csv",
            filetypes=[("Fișiere CSV", "*.csv"), ("Toate fișierele", "*.*")]
        )
        if not filepath_in_dialog: return # Utilizatorul a anulat dialogul de salvare

        try:
            with open(filepath_in_dialog, "w", newline='', encoding='utf-8') as f:
                f.write(csv_content_to_save.strip()) # `strip()` pentru a elimina ultimul newline
            messagebox.showinfo("Succes", f"Datele vizibile au fost exportate cu succes în:\n{filepath_in_dialog}")
            self.refresh_csv_list() # Reîmprospătează lista de fișiere CSV din director, s-ar putea ca noul fișier să apară
        except Exception as e:
            messagebox.showerror("Eroare Export CSV", f"Nu s-a putut exporta fișierul:\n{e}")


    def show_top_results_window(self):
        """
        Afișează o fereastră nouă cu statistici "Top Rezultate" (ex. viteză maximă, distanță maximă etc.).
        Datele sunt extrase din baza de date.
        """
        top_results_window = tk.Toplevel(self.root) # Creează o fereastră Toplevel
        top_results_window.title("Cele Mai Bune Rezultate")
        top_results_window.geometry("700x500")
        top_results_window.transient(self.root) # O face dependentă de fereastra principală
        top_results_window.grab_set()          # Blochează interacțiunea cu alte ferestre ale aplicației

        text_area = tk.Text(top_results_window, wrap=tk.WORD, font=("Consolas", 10), spacing1=5, spacing3=5)
        text_area.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        text_area.config(state=tk.DISABLED) # Inițial, textul nu poate fi editat

        # Funcții ajutătoare pentru formatarea textului în fereastra de top rezultate
        def add_result_category(category_title):
            text_area.config(state=tk.NORMAL)
            text_area.insert(tk.END, f"--- {category_title} ---\n", ("h2_style",)) # Stil pentru titluri de categorie
            text_area.config(state=tk.DISABLED)

        def add_single_result(label, value_str):
            text_area.config(state=tk.NORMAL)
            text_area.insert(tk.END, f"  {label}: ", ("label_style",)) # Stil pentru etichete
            text_area.insert(tk.END, f"{value_str}\n")
            text_area.config(state=tk.DISABLED)

        def add_top_n_results(query_select_value, query_select_details, order_by_column, category_label, unit="", n=3, reverse_order=True, additional_where=""):
            """Funcție generică pentru a extrage și afișa top N rezultate pentru o categorie."""
            add_result_category(f"{category_label} (Top {n})")
            try:
                where_clause = f"WHERE {query_select_value} IS NOT NULL" # Ignoră valorile NULL
                if additional_where:
                    where_clause += f" AND {additional_where}" # Adaugă condiții suplimentare

                # Construiește query-ul SQL
                full_query = f"SELECT {query_select_value}, {query_select_details} FROM trips {where_clause} ORDER BY {order_by_column} {'DESC' if reverse_order else 'ASC'} LIMIT {n}"
                self.db_cursor.execute(full_query)
                results = self.db_cursor.fetchall()
                if results:
                    for i, res_tuple in enumerate(results):
                        val = res_tuple[0] # Valoarea principală (ex. viteza)
                        # Detaliile suplimentare (ex. timestamp-ul)
                        details_list = [str(d) for d in res_tuple[1:] if d is not None]
                        details = ", ".join(details_list)
                        # Formatează valoarea (cu 2 zecimale dacă e float)
                        display_val = f"{val:.2f}" if isinstance(val, float) else str(val)
                        add_single_result(f"{i+1}", f"{display_val} {unit} (Datat: {details})")
                else:
                    add_single_result("Rezultate", "Nicio dată găsită pentru această categorie.")
            except sqlite3.Error as e:
                add_single_result("Eroare BD", str(e))
            text_area.config(state=tk.NORMAL)
            text_area.insert(tk.END, "\n") # Spațiu între categorii
            text_area.config(state=tk.DISABLED)

        # Definirea stilurilor pentru text
        text_area.tag_configure("h2_style", font=("Consolas", 12, "bold"), foreground="navy")
        text_area.tag_configure("label_style", font=("Consolas", 10, "bold"))

        # Afișarea diferitelor categorii de top rezultate
        add_top_n_results("avg_speed", "timestamp_iso", "avg_speed", "Viteză Medie Maximă", "km/h")
        add_top_n_results("max_speed", "timestamp_iso", "max_speed", "Viteză Instantanee Maximă", "km/h")
        add_top_n_results("distance_km", "timestamp_iso", "distance_km", "Distanță Maximă Parcursă", "km")
        add_top_n_results("duration_s", "timestamp_iso", "duration_s", "Cea Mai Lungă Durată de Tură", "sec")
        # Pentru viteza minimă, căutăm cele mai mici valori (reverse_order=False) peste un prag (0.1 km/h)
        add_top_n_results("min_speed", "timestamp_iso", "min_speed", "Viteză Minimă Înregistrată (peste 0.1 km/h)", "km/h", n=3, reverse_order=False, additional_where="min_speed > 0.1")


    def on_closing(self):
        """
        Callback apelat la închiderea ferestrei principale.
        Salvează configurația, închide conexiunea la BD și la portul serial.
        """
        self.arduino_log_message("Închidere aplicație...")
        self.save_config() # Salvează ultima configurație

        if self.db_conn:
            try:
                self.db_conn.close()
                print("Conexiune la baza de date închisă.")
            except Exception as e:
                print(f"Eroare la închiderea bazei de date: {e}")

        # Asigură oprirea corectă a thread-ului de citire și închiderea portului serial
        if self.is_connected:
            self.is_reading_data = False # Semnal pentru oprirea thread-ului
            if hasattr(self, 'read_thread') and self.read_thread.is_alive():
                self.read_thread.join(timeout=0.5) # Așteaptă puțin thread-ul

            if self.serial_connection and self.serial_connection.is_open:
                try:
                    self.serial_connection.close()
                    print("Portul serial a fost închis.")
                except Exception as e:
                    print(f"Eroare la închiderea portului serial: {e}")
            self.is_connected = False
            self.serial_connection = None

        if self.root: # Verifică dacă fereastra root mai există înainte de a o distruge
            self.root.destroy()
        print("Aplicație închisă.")

if __name__ == "__main__":
    root = tk.Tk()
    app = ArduinoTripsApp(root)
    root.mainloop()
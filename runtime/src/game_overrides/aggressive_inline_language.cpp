// Aggressive Inline (SLUS-20327): French, German and Spanish from the Xbox
// release. See aggressive_inline_language.h.

#include "aggressive_inline_language.h"

#include "ps2_runtime.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_xbox_disc.h"
#include "runtime/ps2_zaxis_text.h"
#include "runtime/ps2_zaxis_ui.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace aggressive_inline::language
{
    // Named, not anonymous: the unity build may put other game override
    // files in the same translation unit.
    namespace detail
    {
        namespace zt = ps2x::zaxis_text;
        namespace zx = ps2x::zaxis;
        namespace gf = ps2x::game_files;

        struct Tables
        {
            Language language = Language::English;
            std::atomic<bool> active{false};
            std::once_flag built;
            std::shared_ptr<const zt::Translation> table;
            std::mutex tableMutex;
            std::mutex traceMutex;
            std::set<std::string> traced; // PS2X_TRACE_LANGUAGE: texts shown once
        };
        Tables &tables() { return ps2x::session::state<Tables>(); }

        const char *extension(Language language)
        {
            switch (language)
            {
            case Language::French:
                return "fre";
            case Language::German:
                return "ger";
            case Language::Spanish:
                return "spa";
            default:
                return "eus";
            }
        }

        // The Xbox level scripts' extension for a language.
        const char *levelExtension(Language language)
        {
            switch (language)
            {
            case Language::French:
                return "bnf";
            case Language::German:
                return "bng";
            case Language::Spanish:
                return "bns";
            default:
                return "bni";
            }
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool endsWith(const std::string &text, const std::string &suffix)
        {
            return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        struct Disc
        {
            std::unique_ptr<gf::GameFileSystem> files;
        };
        Disc &disc() { return ps2x::session::state<Disc>(); }

        gf::GameFileSystem &files()
        {
            if (!disc().files)
            {
                const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
                gf::GameFileRoots roots;
                if (!paths.cdFromImage)
                    roots.disc = paths.cdRoot;
                if (roots.disc.empty())
                    roots.image = PS2Runtime::cdImageFileSystem();
                // No mods folder: the English the table is made from is the
                // disc's.
                disc().files = std::make_unique<gf::GameFileSystem>(std::move(roots));
            }
            return *disc().files;
        }

        bool readPs2(const std::string &path, std::vector<uint8_t> &bytes)
        {
            std::shared_ptr<gf::GameFile> file = files().open(path);
            return file && gf::readAll(*file, bytes);
        }

        // ---- the port's own words -------------------------------------------------

        struct PortText
        {
            const char *english;
            const char *french;
            const char *german;
            const char *spanish;
        };

        // UTF-8 here; Latin-1 in the table. Rows of the Settings pages
        // (labels, values, sections) and the menu items the port adds.
        constexpr PortText kPortText[] = {
            {"SETTINGS", "PARAMETRES", "EINSTELLUNGEN", "AJUSTES"},
            {"Settings", "Paramètres", "Einstellungen", "Ajustes"},
            {"Display", "Affichage", "Anzeige", "Pantalla"},
            {"Graphics", "Graphismes", "Grafik", "Gráficos"},
            {"Sound", "Son", "Sound", "Sonido"},
            {"Controls", "Commandes", "Steuerung", "Controles"},
            {"Camera", "Caméra", "Kamera", "Cámara"},
            {"Assists", "Aides", "Hilfen", "Ayudas"},
            {"Cheats", "Codes", "Cheats", "Trucos"},
            {"Gameplay", "Jouabilité", "Spielweise", "Jugabilidad"},
            {"Game", "Jeu", "Spiel", "Juego"},
            // The headings inside a page (capitals, as the games write their titles; no accents in them, as PARAMETRES).
            {"CAMERA", "CAMERA", "KAMERA", "CAMARA"},
            {"ASSISTS", "AIDES", "HILFEN", "AYUDAS"},
            {"CHEATS", "CODES", "CHEATS", "TRUCOS"},
            {"KEYS", "TOUCHES", "TASTATUR", "TECLAS"},
            {"BUTTONS", "BOUTONS", "GAMEPAD", "BOTONES"},
            {"EXTRAS", "EXTRAS", "EXTRAS", "EXTRAS"},
            {"Change", "Modifier", "Ändern", "Cambiar"},
            {"Section", "Section", "Bereich", "Sección"},
            {"Back", "Retour", "Zurück", "Atrás"},
            {"Quit", "Quitter", "Beenden", "Salir"},
            {"Quit to Desktop", "Retour au bureau", "Zum Desktop", "Ir al escritorio"},
            {"Quit to Desktop?", "Retour au bureau ?", "Zum Desktop?", "¿Ir al escritorio?"},
            // The same items and question in the collection (back to its menu).
            {"Exit to Collection", "Retour à la collection", "Zur Sammlung", "Ir a la colección"},
            {"Exit to Collection?", "Retour à la collection ?", "Zur Sammlung?", "¿Ir a la colección?"},
            {"Exit Game", "Quitter la partie", "Spiel beenden", "Salir del juego"},
            {"Restart Level", "Rejouer le niveau", "Level neu starten", "Reiniciar nivel"},
            {"Restart Run", "Rejouer le run", "Lauf neu starten", "Reiniciar carrera"},
            {"Retry Challenge", "Réessayer le défi", "Herausforderung wiederholen", "Reintentar desafío"},
            {"Restart Lesson", "Recommencer la leçon", "Lektion neu starten", "Reiniciar lección"},
            {"Restart", "Recommencer", "Neustart", "Reiniciar"},
            {"On", "Oui", "An", "Sí"},
            {"Off", "Non", "Aus", "No"},
            {"Display Mode", "Mode d'affichage", "Anzeigemodus", "Modo de pantalla"},
            {"Window", "Fenêtre", "Fenster", "Ventana"},
            {"Full Screen", "Plein écran", "Vollbild", "Pantalla completa"},
            {"Borderless", "Sans bordure", "Randlos", "Sin bordes"},
            {"Renderer", "Rendu", "Renderer", "Renderizador"},
            {"Resolution", "Résolution", "Auflösung", "Resolución"},
            {"Native", "Native", "Nativ", "Nativa"},
            {"Full Height", "Pleine hauteur", "Volle Höhe", "Altura completa"},
            {"Widescreen", "Écran large", "Breitbild", "Panorámico"},
            {"Wide Aspect", "Format large", "Breitformat", "Formato panorámico"},
            {"Motion Blur", "Flou de mouvement", "Bewegungsunschärfe", "Desenfoque"},
            {"Low", "Faible", "Gering", "Bajo"},
            {"Original", "Original", "Original", "Original"},
            {"Draw Distance", "Distance d'affichage", "Sichtweite", "Distancia de dibujado"},
            {"Far", "Loin", "Weit", "Lejos"},
            {"Farthest", "Très loin", "Am weitesten", "Muy lejos"},
            {"Scaling", "Mise à l'échelle", "Skalierung", "Escalado"},
            {"Smooth", "Lisse", "Weich", "Suave"},
            {"Integer", "Entière", "Ganzzahlig", "Entero"},
            {"Show Frame Rate", "Afficher les FPS", "Bildrate zeigen", "Mostrar FPS"},
            {"Pause in Background", "Pause en arrière-plan", "Pause im Hintergrund", "Pausa en segundo plano"},
            {"Master Volume", "Volume général", "Gesamtlautstärke", "Volumen general"},
            {"Mute in Background", "Muet en arrière-plan", "Stumm im Hintergrund", "Silencio en segundo plano"},
            {"Movie Volume", "Volume des vidéos", "Videolautstärke", "Volumen de vídeos"},
            {"Music Volume", "Volume musique", "Musiklautstärke", "Volumen música"},
            {"Effects Volume", "Volume effets", "Effektlautstärke", "Volumen efectos"},
            {"Speech Volume", "Volume voix", "Sprachlautstärke", "Volumen voces"},
            {"Skip Intro Movies", "Passer les vidéos d'intro", "Intro-Videos überspringen", "Saltar vídeos de inicio"},
            {"Developer Menu", "Menu développeur", "Entwicklermenü", "Menú de desarrollo"},
            {"Debug Menu", "Menu de débogage", "Debug-Menü", "Menú de depuración"},
            {"Language", "Langue", "Sprache", "Idioma"},
            {"French Voices", "Voix françaises", "Französische Stimmen", "Voces en francés"},
            {"Mouse Sensitivity", "Sensibilité souris", "Mausempfindlichkeit", "Sensibilidad del ratón"},
            {"Invert Mouse Y", "Inverser l'axe Y", "Maus-Y umkehren", "Invertir eje Y"},
            {"Mouse Look", "Vue à la souris", "Mausblick", "Vista con ratón"},
            {"Free Look", "Vue libre", "Freie Sicht", "Vista libre"},
            {"Stick Dead Zone", "Zone morte du stick", "Stick-Totzone", "Zona muerta del stick"},
            {"Gamepad", "Manette", "Gamepad", "Mando"},
            {"Button Prompts", "Icônes des touches", "Tastensymbole", "Iconos de botones"},
            {"Keyboard", "Clavier", "Tastatur", "Teclado"},
            {"Reset Controls", "Commandes par défaut", "Steuerung zurücksetzen", "Restablecer controles"},
            {"Forward", "Avancer", "Vorwärts", "Avanzar"},
            {"Brake", "Freiner", "Bremsen", "Frenar"},
            {"Turn Left", "Tourner à gauche", "Links drehen", "Girar a la izquierda"},
            {"Turn Right", "Tourner à droite", "Rechts drehen", "Girar a la derecha"},
            {"Half Push", "Demi-poussée", "Halber Schub", "Medio impulso"},
            {"Jump", "Sauter", "Springen", "Saltar"},
            {"Grab and Trick", "Grab et trick", "Grab und Trick", "Grab y truco"},
            {"Action", "Action", "Aktion", "Acción"},
            {"Rotate Left", "Rotation gauche", "Links rotieren", "Rotar izquierda"},
            {"Rotate Right", "Rotation droite", "Rechts rotieren", "Rotar derecha"},
            {"Cess Slide Left", "Cess slide gauche", "Cess Slide links", "Cess slide izquierda"},
            {"Cess Slide Right", "Cess slide droite", "Cess Slide rechts", "Cess slide derecha"},
            {"Pause", "Pause", "Pause", "Pausa"},
            {"Challenges", "Défis", "Herausforderungen", "Desafíos"},
            {"Camera Behind", "Caméra derrière", "Kamera hinten", "Cámara detrás"},
            {"Menu Up", "Menu haut", "Menü hoch", "Menú arriba"},
            {"Menu Down", "Menu bas", "Menü runter", "Menú abajo"},
            {"Menu Left", "Menu gauche", "Menü links", "Menú izquierda"},
            {"Menu Right", "Menu droite", "Menü rechts", "Menú derecha"},
            {"Hide HUD", "Masquer l'interface", "HUD ausblenden", "Ocultar HUD"},
            {"Screenshot", "Capture d'écran", "Screenshot", "Captura"},
            {"Field of View", "Champ de vision", "Sichtfeld", "Campo de visión"},
            {"Camera Distance", "Distance caméra", "Kameraabstand", "Distancia de cámara"},
            {"Camera Height", "Hauteur caméra", "Kamerahöhe", "Altura de cámara"},
            {"Camera Style", "Style de caméra", "Kamerastil", "Estilo de cámara"},
            {"Out of Juice", "Plus d'endurance", "Kein Juice mehr", "Sin fuerza"},
            {"Run Ends", "Fin du run", "Lauf endet", "Fin de carrera"},
            {"Keep Skating", "Continuer", "Weiterskaten", "Seguir patinando"},
            {"Juice Drain", "Perte d'endurance", "Juice-Verbrauch", "Pérdida de fuerza"},
            {"Juice Lost on Crash", "Endurance perdue (chute)", "Juice-Verlust bei Sturz", "Fuerza perdida al caer"},
            {"Low Gravity Wall Ride", "Wall ride gravité réduite", "Low Gravity Wall Ride", "Wallride baja gravedad"},
            {"Juice Regeneration", "Régénération endurance", "Juice-Regeneration", "Regeneración de fuerza"},
            {"Invulnerability", "Invulnérabilité", "Unverwundbarkeit", "Invulnerabilidad"},
            {"Perfect Grind", "Grind parfait", "Perfekter Grind", "Grind perfecto"},
            {"Perfect Handplant", "Handplant parfait", "Perfekter Handplant", "Handplant perfecto"},
            {"Super Spin", "Super spin", "Super Spin", "Super giro"},
            {"Perfect Manual", "Manual parfait", "Perfekter Manual", "Manual perfecto"},
            {"Unlock Everything", "Tout débloquer", "Alles freischalten", "Desbloquear todo"},
            {"Sure?", "Sûr ?", "Sicher?", "¿Seguro?"},
            {"Unlocked", "Débloqué", "Freigeschaltet", "Desbloqueado"},
            {"Zoo Level", "Niveau Zoo", "Zoo-Level", "Nivel Zoo"},
            {"Estate Level", "Niveau Estate", "Estate-Level", "Nivel Estate"},
            {"Pad", "Manette", "Gamepad", "Mando"},
            {"Press a key", "Appuyez sur une touche", "Taste drücken", "Pulsa una tecla"},
            // The Tutorial loading screens drawn for Nintendo-style pads (aggressive_inline_prompts.cpp).
            {"Rotate L / R", "Rotation G / D", "Drehen L / R", "Girar I / D"},
            {"Cess slide", "Cess slide", "Cess Slide", "Cess slide"},
            {"Grind", "Grind", "Grind", "Grind"},
            {"Jump / Accelerate", "Saut / Accélérer", "Springen / Beschleunigen", "Saltar / Acelerar"},
            {"Grabs and Tricks", "Grabs et figures", "Grabs und Tricks", "Grabs y trucos"},
            {"Left", "Gauche", "Links", "Izquierda"},
            {"Right", "Droite", "Rechts", "Derecha"},
            // Gamepad diagnostics rows (Settings > Controls).
            {"Gamepads", "Manettes", "Gamepads", "Mandos"},
            {"Connected Gamepads", "Manettes connectées", "Verbundene Gamepads", "Mandos conectados"},
            {"Test Rumble", "Tester la vibration", "Vibration testen", "Probar vibración"},
            {"Swap Pads 1 and 2", "Échanger manettes 1 et 2", "Pads 1 und 2 tauschen", "Intercambiar mandos 1 y 2"},
            // Photo mode (the pause item, the key's row, its help in the overlay).
            {"Photo Mode", "Mode photo", "Fotomodus", "Modo foto"},
            {"Look", "Regarder", "Umsehen", "Mirar"},
            {"Move", "Se déplacer", "Bewegen", "Mover"},
            {"Up / Down", "Monter / Descendre", "Hoch / Runter", "Subir / Bajar"},
            {"Roll", "Inclinaison", "Neigen", "Inclinar"},
            {"Zoom", "Zoom", "Zoom", "Zoom"},
            {"Faster / Slower", "Plus vite / Plus lent", "Schneller / Langsamer", "Más rápido / Más lento"},
            {"Faster", "Plus vite", "Schneller", "Más rápido"},
            {"Speed", "Vitesse", "Tempo", "Velocidad"},
            {"Filter", "Filtre", "Filter", "Filtro"},
            {"Reset Camera", "Réinitialiser la caméra", "Kamera zurücksetzen", "Restablecer cámara"},
            {"Save Picture", "Enregistrer l'image", "Bild speichern", "Guardar imagen"},
            {"Hide Help", "Masquer l'aide", "Hilfe ausblenden", "Ocultar ayuda"},
            {"Help", "Aide", "Hilfe", "Ayuda"},
            {"Mouse", "Souris", "Maus", "Ratón"},
            {"Wheel", "Molette", "Mausrad", "Rueda"},
            {"Left Stick", "Stick gauche", "Linker Stick", "Stick izquierdo"},
            {"Right Stick", "Stick droit", "Rechter Stick", "Stick derecho"},
            {"D-pad Up / Down", "Croix haut / bas", "Steuerkreuz hoch / runter", "Cruceta arriba / abajo"},
            {"D-pad Left / Right", "Croix gauche / droite", "Steuerkreuz links / rechts", "Cruceta izquierda / derecha"},
            {"None", "Aucun", "Keiner", "Ninguno"},
            {"Letterbox", "Bandes noires", "Letterbox", "Bandas negras"},
            {"Vignette", "Vignettage", "Vignette", "Viñeta"},
            {"Black & White", "Noir et blanc", "Schwarzweiß", "Blanco y negro"},
            {"Cinema", "Cinéma", "Kino", "Cine"},
            {"Noir", "Film noir", "Film noir", "Cine negro"},
        };

        // The Settings rows' help lines (the line under the rows) and the
        // restart note added to some of them ("Start at the title screen.
        // Applies at next start.": the table translates sentence by
        // sentence). Kept about as short as the English: the line is drawn
        // narrower when it is longer than the frame.
        constexpr PortText kPortHelp[] = {
            {"Applies the next time the game starts.", "S'applique au prochain démarrage du jeu.",
             "Gilt ab dem nächsten Spielstart.", "Se aplica la próxima vez que se inicie el juego."},
            {"Applies at next start.", "Au prochain démarrage.", "Ab dem nächsten Start.", "Al próximo inicio."},
            // Controls: the gamepad rows.
            {"Player 1 first.", "Joueur 1 d'abord.",
             "Spieler 1 zuerst.", "Jugador 1 primero."},
            {"Rumbles each gamepad.", "Fait vibrer chaque manette.",
             "Lässt jedes Gamepad vibrieren.", "Hace vibrar cada mando."},
            {"Players 1 and 2 trade gamepads.", "Les joueurs 1 et 2 échangent leurs manettes.",
             "Spieler 1 und 2 tauschen die Gamepads.", "Los jugadores 1 y 2 intercambian mandos."},
            // Display
            {"Borderless stays in the same Space.", "Sans bordure reste sur le même bureau.",
             "Randlos bleibt auf demselben Schreibtisch.", "Sin bordes se queda en el mismo escritorio."},
            {"CPU is most accurate, GPU sharper.", "CPU plus fidèle, GPU plus net.",
             "CPU am genauesten, GPU schärfer.", "CPU más fiel, GPU más nítido."},
            {"640x448 times this.", "640x448 multiplié par ceci.",
             "640x448 mal diesen Wert.", "640x448 por este valor."},
            {"A sharper picture.", "Une image plus nette.",
             "Ein schärferes Bild.", "Una imagen más nítida."},
            {"16:9 in levels, 4:3 menus and movies.", "16:9 dans les niveaux, menus et vidéos en 4:3.",
             "16:9 in Levels, Menüs und Videos in 4:3.", "16:9 en los niveles, menús y vídeos en 4:3."},
            {"Window matches the window's shape.", "Fenêtre suit la forme de la fenêtre.",
             "Fenster folgt der Form des Fensters.", "Ventana sigue la forma de la ventana."},
            {"The blur trail in levels.", "La traînée de flou dans les niveaux.",
             "Die Unschärfespur in Levels.", "El rastro de desenfoque en los niveles."},
            {"Integer keeps pixels sharp.", "Entière garde les pixels nets.",
             "Ganzzahlig hält Pixel scharf.", "Entero mantiene los píxeles nítidos."},
            {"Off lowers latency but may tear.", "Non réduit la latence, déchirures possibles.",
             "Aus senkt die Latenz, Bildrisse möglich.", "No reduce la latencia, puede cortar la imagen."},
            {"Frames per second, top right.", "Images par seconde, en haut à droite.", "Bilder pro Sekunde, oben rechts.",
             "Imágenes por segundo, arriba a la derecha."},
            {"Pauses when you switch away.", "Pause quand vous changez de fenêtre.",
             "Pausiert, wenn du wegwechselst.", "Pausa al cambiar de ventana."},
            // Sound
            {"All sound.", "Tout le son.", "Der gesamte Ton.", "Todo el sonido."},
            {"Mutes when you switch away.", "Muet quand vous changez de fenêtre.",
             "Stumm, wenn du wegwechselst.", "Silencio al cambiar de ventana."},
            {"Intro and menu movies.", "Vidéos d'intro et des menus.",
             "Intro- und Menüvideos.", "Vídeos de inicio y de menús."},
            {"The game's own music volume.", "Le volume musique du jeu.",
             "Die Musiklautstärke des Spiels.", "El volumen de música del juego."},
            {"The game's own effects volume.", "Le volume effets du jeu.",
             "Die Effektlautstärke des Spiels.", "El volumen de efectos del juego."},
            {"The game's own speech volume.", "Le volume voix du jeu.",
             "Die Sprachlautstärke des Spiels.", "El volumen de voces del juego."},
            {"Speech in French.", "Voix en français.",
             "Sprache auf Französisch.", "Voces en francés."},
            // Controls
            {"How far the camera turns with the mouse.", "De combien la caméra tourne avec la souris.",
             "Wie weit die Kamera mit der Maus dreht.", "Cuánto gira la cámara con el ratón."},
            {"Mouse up looks down.", "Souris vers le haut, vue vers le bas.",
             "Maus hoch blickt nach unten.", "Ratón arriba, vista abajo."},
            {"The mouse turns the view or moves the stick.", "La souris tourne la vue ou déplace le stick.",
             "Die Maus dreht die Sicht oder bewegt den Stick.", "El ratón gira la vista o mueve el stick."},
            {"Smaller stick pushes are ignored.", "Les petits mouvements du stick sont ignorés.",
             "Kleinere Stick-Bewegungen werden ignoriert.", "Se ignoran los movimientos pequeños del stick."},
            {"Off ignores connected gamepads.", "Non ignore les manettes branchées.",
             "Aus ignoriert angeschlossene Gamepads.", "No ignora los mandos conectados."},
            {"Auto matches the device in use.", "Auto suit l'appareil utilisé.",
             "Auto folgt dem benutzten Gerät.", "Auto sigue el dispositivo en uso."},
            {"Cross, then a key. Esc cancels, Backspace clears.", "Croix, puis une touche. Échap annule, Retour arrière efface.",
             "Kreuz, dann eine Taste. Esc bricht ab, Rücktaste löscht.", "Botón X, luego una tecla. Esc cancela, Retroceso borra."},
            {"Cross, then a pad button. Esc cancels.", "Croix, puis un bouton de la manette. Échap annule.",
             "Kreuz, dann eine Gamepad-Taste. Esc bricht ab.", "Botón X, luego un botón del mando. Esc cancela."},
            {"All controls back to the defaults.", "Toutes les commandes par défaut.",
             "Alle Steuerungen auf Standard.", "Todos los controles por defecto."},
            // Camera
            {"Wider shows more. The game's is 75.", "Plus large, on voit plus. Celui du jeu : 75.",
             "Breiter zeigt mehr. Im Spiel 75.", "Más amplio muestra más. El del juego es 75."},
            {"How far behind the skater.", "Distance derrière le skateur.",
             "Wie weit hinter dem Skater.", "Distancia detrás del patinador."},
            {"How high above the skater.", "Hauteur au-dessus du skateur.",
             "Wie hoch über dem Skater.", "Altura sobre el patinador."},
            {"Xbox shows more in widescreen.", "Xbox montre plus en écran large.",
             "Xbox zeigt mehr im Breitbild.", "Xbox muestra más en panorámico."},
            // Assists
            {"What happens when the juice runs out.", "Ce qui se passe sans endurance.",
             "Was passiert, wenn der Juice leer ist.", "Qué pasa cuando se acaba la fuerza."},
            {"How fast the juice meter empties.", "Vitesse à laquelle la jauge d'endurance se vide.",
             "Wie schnell die Juice-Anzeige leer wird.", "Lo rápido que se vacía el medidor de fuerza."},
            {"Juice a crash takes away.", "L'endurance qu'une chute fait perdre.", "Juice, das ein Sturz kostet.",
             "La fuerza que quita una caída."},
            // Cheats
            {"Wall rides last longer.", "Les wall rides durent plus longtemps.", "Wallrides dauern länger.",
             "Los wallrides duran más."},
            {"The juice meter fills up by itself.", "La jauge d'endurance se remplit toute seule.",
             "Die Juice-Anzeige füllt sich von selbst.", "El medidor de fuerza se llena solo."},
            {"Crashes cost no juice.", "Les chutes ne coûtent pas d'endurance.", "Stürze kosten kein Juice.",
             "Las caídas no quitan fuerza."},
            {"Grinds never lose balance.", "Les grinds ne perdent jamais l'équilibre.",
             "Grinds verlieren nie das Gleichgewicht.", "Los grinds nunca pierden el equilibrio."},
            {"Hand plants never lose balance.", "Les hand plants ne perdent jamais l'équilibre.",
             "Handplants verlieren nie das Gleichgewicht.", "Los hand plants nunca pierden el equilibrio."},
            {"Spins twice as fast in the air.", "Rotations deux fois plus rapides en l'air.",
             "Doppelt so schnelle Drehungen in der Luft.", "Giros el doble de rápidos en el aire."},
            {"Manuals never lose balance.", "Les manuals ne perdent jamais l'équilibre.",
             "Manuals verlieren nie das Gleichgewicht.", "Los manuals nunca pierden el equilibrio."},
            {"Opens all levels, skaters and movies.", "Débloque niveaux, skateurs et vidéos.",
             "Schaltet Levels, Skater und Videos frei.", "Desbloquea niveles, patinadores y vídeos."},
            // Game
            {"Start at the title screen.", "Démarrer sur l'écran titre.", "Beim Titelbildschirm starten.",
             "Empezar en la pantalla de título."},
            {"Menus, challenges and tips.", "Menus, défis et conseils.",
             "Menüs, Herausforderungen und Tipps.", "Menús, desafíos y consejos."},
            {"Press F1 in a level.", "Appuyez sur F1 dans un niveau.",
             "F1 in einem Level drücken.", "Pulsa F1 en un nivel."},
            {"The cut level, in Free Skate and Timed Run.", "Le niveau coupé, en Freeskate et Parcours 2 min.",
             "Der gestrichene Level, in Freeskate und Zeitrennen.", "El nivel eliminado, en Patinaje Libre y Contrarreloj."},
            {"An early Tutorial, in Free Skate.", "Un ancien Tutoriel, en Freeskate.",
             "Ein frühes Tutorial, in Freeskate.", "Un Tutorial anterior, en Patinaje Libre."},
            {"Opens the game's debug menu in a level.", "Ouvre le menu de débogage du jeu dans un niveau.",
             "Öffnet das Debug-Menü des Spiels in einem Level.", "Abre el menú de depuración del juego en un nivel."},
        };

        const char *portText(const PortText &row, Language language)
        {
            switch (language)
            {
            case Language::French:
                return row.french;
            case Language::German:
                return row.german;
            case Language::Spanish:
                return row.spanish;
            default:
                return row.english;
            }
        }

        // ---- the table ----------------------------------------------------------

        std::shared_ptr<zt::Translation> buildTable(Language language)
        {
            auto table = std::make_shared<zt::Translation>();
            size_t fromStrings = 0, fromLevels = 0;
            // The Xbox string tables, key by key.
            std::vector<uint8_t> bytes;
            zt::StringTable english, other;
            if (ps2_xbox_disc::readFile("string.eus", bytes) && zt::readStringTable(bytes, english) &&
                ps2_xbox_disc::readFile(std::string("string.") + extension(language), bytes) &&
                zt::readStringTable(bytes, other))
            {
                for (const auto &[key, text] : english.entries)
                    if (const std::string *translated = other.find(key))
                    {
                        const size_t before = table->size();
                        table->add(text, *translated);
                        fromStrings += table->size() - before;
                    }
            }
            else
            {
                std::cerr << "[language] Xbox string tables not found" << std::endl;
            }
            // The level scripts: every <name>.bni with its translation, and
            // the PS2 script of the same name against the .bni.
            const std::string ext = levelExtension(language);
            for (const std::string &level : ps2_xbox_disc::list("levels"))
            {
                const std::string dir = "levels/" + level + "/";
                for (const std::string &name : ps2_xbox_disc::list(dir))
                {
                    // tut_xbox and tut_gc are the Tutorial for other pads;
                    // the PS2's is tut.
                    const std::string lowered = lower(name);
                    if (!endsWith(lowered, ".bni") || lowered.find("_xbox") != std::string::npos ||
                        lowered.find("_gc") != std::string::npos)
                        continue;
                    const std::string base = name.substr(0, name.size() - 4);
                    std::vector<uint8_t> a, b, p;
                    zt::LevelText textA, textB, textP;
                    if (!ps2_xbox_disc::readFile(dir + name, a) || !ps2_xbox_disc::readFile(dir + base + "." + ext, b) ||
                        !zt::readLevelText(a, textA) || !zt::readLevelText(b, textB))
                        continue;
                    const auto pairs = zt::pairLevelText(textA, textB);
                    for (const auto &[en, tr] : pairs)
                    {
                        if (en == "none")
                            continue;
                        const size_t before = table->size();
                        table->add(en, tr);
                        fromLevels += table->size() - before;
                    }
                    // The PS2's own wording where it differs from the Xbox's,
                    // and the Xbox English script's (the career scripts
                    // setting runs it; .bni is an older copy of it).
                    std::vector<uint8_t> xboxBin;
                    zt::LevelText textX;
                    for (int which = 0; which < 2; ++which)
                    {
                        const bool ok = which == 0 ? readPs2("ASSETS/LEVELS/" + level + "/" + base + ".BIN", p) &&
                                                         zt::readLevelText(p, textP)
                                                   : ps2_xbox_disc::readFile(dir + base + ".bin", xboxBin) &&
                                                         zt::readLevelText(xboxBin, textX);
                        if (!ok)
                            continue;
                        const zt::LevelText &other = which == 0 ? textP : textX;
                        for (const auto &[ps2, xboxEnglish] : zt::pairLevelText(other, textA))
                        {
                            if (ps2 == xboxEnglish || table->find(ps2))
                                continue;
                            if (const std::string *tr = table->find(xboxEnglish))
                            {
                                const std::string copy = *tr;
                                table->add(ps2, copy);
                                ++fromLevels;
                            }
                        }
                    }
                }
            }
            // The port's words.
            // (after the game's own, which win: "Jump" is the attribute's
            // word in the game).
            for (const PortText &row : kPortText)
                table->add(row.english, zt::latin1FromUtf8(portText(row, language)));
            // The help lines: the port's wording even where a game text
            // has the same English.
            for (const PortText &row : kPortHelp)
                table->set(row.english, zt::latin1FromUtf8(portText(row, language)));
            std::cout << "[language] " << extension(language) << ": " << table->size() << " texts (" << fromStrings
                      << " from the string tables, " << fromLevels << " from the level scripts, "
                      << std::size(kPortText) + std::size(kPortHelp) << " of the port's)" << std::endl;
            return table;
        }

        std::shared_ptr<const zt::Translation> table()
        {
            std::call_once(tables().built, []()
                           {
                               auto built = buildTable(tables().language);
                               std::lock_guard<std::mutex> lock(tables().tableMutex);
                               tables().table = std::move(built);
                           });
            std::lock_guard<std::mutex> lock(tables().tableMutex);
            return tables().table;
        }

        // ---- assets\string.eus ------------------------------------------------------

        // The PS2 table with the language's text for each key whose English
        // the Xbox table has under the same key (else the text's translation,
        // else the English).
        bool patchStringTable(std::vector<uint8_t> &bytes)
        {
            zt::StringTable ps2, english, other;
            std::vector<uint8_t> x;
            if (!zt::readStringTable(bytes, ps2) || !ps2_xbox_disc::readFile("string.eus", x) ||
                !zt::readStringTable(x, english) ||
                !ps2_xbox_disc::readFile(std::string("string.") + extension(tables().language), x) ||
                !zt::readStringTable(x, other))
                return false;
            const std::shared_ptr<const zt::Translation> words = table();
            size_t changed = 0;
            for (auto &[key, text] : ps2.entries)
            {
                std::string replacement;
                const std::string *xboxEnglish = english.find(key);
                const std::string *translated = other.find(key);
                if (xboxEnglish && translated && *xboxEnglish == text)
                    replacement = *translated;
                else if (words)
                    replacement = words->translate(text);
                if (!replacement.empty() && replacement != text)
                {
                    text = replacement;
                    ++changed;
                }
            }
            if (changed == 0)
                return false;
            bytes = zt::writeStringTable(ps2);
            std::cout << "[language] string.eus: " << changed << " of " << ps2.entries.size() << " texts in "
                      << extension(tables().language) << std::endl;
            return true;
        }
    }

    bool install(PS2Runtime &runtime)
    {
        (void)runtime;
        using namespace detail;
        const char *value = ps2x::boot::text("PS2X_LANGUAGE");
        const std::string name = lower(value ? value : "");
        if (name == "french" || name == "fre" || name == "fr" || name == "francais")
            tables().language = Language::French;
        else if (name == "german" || name == "ger" || name == "de" || name == "deutsch")
            tables().language = Language::German;
        else if (name == "spanish" || name == "spa" || name == "es" || name == "espanol")
            tables().language = Language::Spanish;
        else
            tables().language = Language::English;
        if (tables().language == Language::English)
            return false;
        if (const char *menus = ps2x::boot::text("PS2X_GAME_SETTINGS_MENUS"); !menus || menus[0] != '1')
        {
            std::cerr << "[language] " << name << " needs PS2X_GAME_SETTINGS_MENUS=1 (the app); English" << std::endl;
            return false;
        }
        if (!ps2_xbox_disc::init() || !ps2_xbox_disc::exists(std::string("string.") + extension(tables().language)))
        {
            std::cerr << "[language] no Xbox files (" << ps2_xbox_disc::description() << "): English" << std::endl;
            return false;
        }
        tables().active = true;
        ps2x::disc_patch::registerPatch("STRING.EUS", &patchStringTable, "ai");
        std::cout << "[language] " << name << " from the Xbox release (" << ps2_xbox_disc::description() << ")"
                  << std::endl;
        return true;
    }

    bool active() { return detail::tables().active.load(); }

    Language current() { return detail::tables().active ? detail::tables().language : Language::English; }

    namespace detail
    {
        std::function<bool(const std::string &)> g_keep; // keepUntranslated
    }

    void keepUntranslated(std::function<bool(const std::string &)> keep) { detail::g_keep = std::move(keep); }

    std::string translate(const std::string &english)
    {
        if (!detail::tables().active)
            return {};
        if (detail::g_keep && detail::g_keep(english))
            return {};
        const auto words = detail::table();
        std::string out = words ? words->translate(english) : std::string();
        // PS2X_TRACE_LANGUAGE=1: each text drawn without a translation, once
        // (2: every text drawn, with its translation, once).
        static const int trace = []()
        {
            const char *value = std::getenv("PS2X_TRACE_LANGUAGE");
            return value ? std::atoi(value) : 0;
        }();
        if (trace >= 2 || (trace == 1 && out.empty()))
        {
            detail::Tables &t = detail::tables();
            std::lock_guard<std::mutex> lock(t.traceMutex);
            if (t.traced.insert(english).second)
                std::cout << "[language] " << (out.empty() ? "untranslated: \"" + english + "\""
                                                           : "\"" + english + "\" -> \"" + out + "\"")
                          << std::endl;
        }
        return out;
    }

    bool patchUiLibrary(std::vector<ps2x::zaxis::ArchiveEntry> &entries)
    {
        using namespace detail;
        if (!tables().active)
            return false;
        // Build the table now, before anything is drawn.
        table();
        std::vector<uint8_t> bytes;
        std::vector<zx::ArchiveEntry> xbox;
        std::string error;
        if (!ps2_xbox_disc::readFile("uilib.zal", bytes) || !zx::readArchive(bytes, xbox, &error))
        {
            std::cerr << "[language] Xbox uilib.zal not read " << error << ": PS2 fonts" << std::endl;
            return false;
        }
        bool changed = false;
        // The UI scripts: the Xbox release fits its longer texts by
        // narrowing the widgets that hold them (the Tutorial's lesson list
        // in the pause menu at 0.8 of its width, the Trick List and Sound
        // Options titles); the PS2 widgets of the same path take the Xbox's
        // width (zaxis::narrowLike).
        for (zx::ArchiveEntry &entry : entries)
        {
            const std::string script = zx::scriptName(entry);
            if (script.empty())
                continue;
            for (const zx::ArchiveEntry &donor : xbox)
            {
                if (donor.nameHash != entry.nameHash || zx::scriptName(donor) != script)
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                const std::string theirs(donor.data.begin(), donor.data.end());
                const std::vector<std::string> narrowed = zx::narrowLike(text, theirs);
                for (const std::string &path : narrowed)
                    std::cout << "[language] " << script << ": " << path << " as narrow as the Xbox's" << std::endl;
                if (!narrowed.empty())
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                }
                break;
            }
        }
        for (zx::ArchiveEntry &entry : entries)
        {
            std::vector<zx::LibraryFont> fonts;
            if (!zx::readLibrary(entry.data, nullptr, &fonts) || fonts.empty())
                continue;
            for (const zx::ArchiveEntry &donor : xbox)
            {
                if (donor.nameHash != entry.nameHash)
                    continue;
                const size_t before = entry.data.size();
                const std::vector<std::string> replaced = zx::replaceFonts(entry.data, donor.data);
                if (replaced.empty())
                    continue;
                changed = true;
                std::cout << "[language] fonts from the Xbox:";
                for (const std::string &font : replaced)
                    std::cout << " " << font;
                std::cout << " (" << before << " -> " << entry.data.size() << " bytes)" << std::endl;
            }
        }
        return changed;
    }

    bool frenchVoices()
    {
        if (ps2x::settings::has("audio.french_voices"))
            return ps2x::settings::value("audio.french_voices", 0) != 0;
        if (const char *value = ps2x::boot::text("PS2X_FRENCH_VOICES"); value && *value)
            return value[0] == '1';
        return current() == Language::French;
    }
}

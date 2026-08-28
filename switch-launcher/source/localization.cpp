#include "localization.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace LauncherLocalization
{
namespace
{
constexpr std::array<Language, 7> LANGUAGE_LIST = {{
    {"system", "System"}, {"en", "English"}, {"fr", "Français"}, {"de", "Deutsch"},
    {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"},
}};

struct Entry
{
  const char* source;
  const char* fr;
  const char* de;
  const char* es;
  const char* it;
  const char* pt;
};

// Keep product names and technical terms in English when translating them would
// make the UI less precise. These strings are launcher-owned UI only; game
// names, paths, network errors and user-entered text never pass through here.
constexpr Entry ENTRIES[] = {
    {"System", "Système", "System", "Sistema", "Sistema", "Sistema"},
    {"Language", "Langue", "Sprache", "Idioma", "Lingua", "Idioma"},
    {"Launcher", "Lanceur", "Launcher", "Lanzador", "Launcher", "Launcher"},
    {"Settings", "Paramètres", "Einstellungen", "Ajustes", "Impostazioni", "Definições"},
    {"Global settings", "Paramètres globaux", "Globale Einstellungen", "Ajustes globales", "Impostazioni globali", "Definições globais"},
    {"Per-game settings", "Paramètres par jeu", "Spielspezifische Einstellungen", "Ajustes por juego", "Impostazioni per gioco", "Definições por jogo"},
    {"Library & storage", "Bibliothèque et stockage", "Bibliothek & Speicher", "Biblioteca y almacenamiento", "Libreria e archiviazione", "Biblioteca e armazenamento"},
    {"File manager", "Gestionnaire de fichiers", "Dateimanager", "Gestor de archivos", "Gestione file", "Gestor de ficheiros"},
    {"Game folders", "Dossiers de jeux", "Spieleordner", "Carpetas de juegos", "Cartelle dei giochi", "Pastas de jogos"},
    {"SMB network shares", "Partages réseau SMB", "SMB-Netzwerkfreigaben", "Recursos de red SMB", "Condivisioni di rete SMB", "Partilhas de rede SMB"},
    {"Theme", "Thème", "Design", "Tema", "Tema", "Tema"},
    {"Games per row", "Jeux par ligne", "Spiele pro Zeile", "Juegos por fila", "Giochi per riga", "Jogos por linha"},
    {"Rows per page", "Lignes par page", "Zeilen pro Seite", "Filas por página", "Righe per pagina", "Linhas por página"},
    {"Show game titles", "Afficher les titres", "Spieltitel anzeigen", "Mostrar títulos", "Mostra i titoli", "Mostrar títulos"},
    {"Show region flags", "Afficher les drapeaux de région", "Regionsflaggen anzeigen", "Mostrar banderas de región", "Mostra bandiere regionali", "Mostrar bandeiras de região"},
    {"Show custom settings badges", "Afficher les indicateurs de paramètres personnalisés", "Markierungen für benutzerdefinierte Einstellungen anzeigen", "Mostrar indicadores de ajustes personalizados", "Mostra indicatori delle impostazioni personalizzate", "Mostrar indicadores de definições personalizadas"},
    {"UI animations", "Animations de l'interface", "UI-Animationen", "Animaciones de la interfaz", "Animazioni dell'interfaccia", "Animações da interface"},
    {"Sound effects", "Effets sonores", "Soundeffekte", "Efectos de sonido", "Effetti sonori", "Efeitos sonoros"},
    {"Check updates at boot", "Vérifier au démarrage", "Beim Start nach Updates suchen", "Buscar actualizaciones al iniciar", "Controlla aggiornamenti all'avvio", "Procurar atualizações ao iniciar"},
    {"Check for updates", "Rechercher des mises à jour", "Nach Updates suchen", "Buscar actualizaciones", "Controlla aggiornamenti", "Procurar atualizações"},
    {"Check for Updates", "Rechercher des mises à jour", "Nach Updates suchen", "Buscar actualizaciones", "Controlla aggiornamenti", "Procurar atualizações"},
    {"Download covers", "Télécharger les jaquettes", "Cover herunterladen", "Descargar carátulas", "Scarica copertine", "Transferir capas"},
    {"Cover settings", "Paramètres de la jaquette", "Cover-Einstellungen", "Ajustes de la carátula", "Impostazioni copertina", "Definições da capa"},
    {"Download from SteamGridDB", "Télécharger depuis SteamGridDB", "Von SteamGridDB herunterladen", "Descargar desde SteamGridDB", "Scarica da SteamGridDB", "Transferir do SteamGridDB"},
    {"Import cover from file", "Importer une jaquette depuis un fichier", "Cover aus Datei importieren", "Importar carátula desde un archivo", "Importa copertina da file", "Importar capa de um ficheiro"},
    {"Online artwork", "Illustration en ligne", "Online-Cover", "Ilustración en línea", "Immagine online", "Imagem online"},
    {"Local image", "Image locale", "Lokales Bild", "Imagen local", "Immagine locale", "Imagem local"},
    {"Search SteamGridDB and replace this game's custom cover with selected online artwork.", "Recherche sur SteamGridDB et remplace la jaquette personnalisée de ce jeu par l'illustration sélectionnée.", "Durchsucht SteamGridDB und ersetzt das benutzerdefinierte Cover dieses Spiels durch das ausgewählte Online-Bild.", "Busca en SteamGridDB y sustituye la carátula personalizada de este juego por la ilustración seleccionada.", "Cerca su SteamGridDB e sostituisce la copertina personalizzata del gioco con l'immagine selezionata.", "Pesquisa no SteamGridDB e substitui a capa personalizada deste jogo pela imagem selecionada."},
    {"Choose a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage. It is validated and saved safely as PNG.", "Choisissez une image PNG, JPEG, WebP ou BMP sur un stockage SD, USB ou SMB. Elle est vérifiée et enregistrée en toute sécurité au format PNG.", "Wähle ein PNG-, JPEG-, WebP- oder BMP-Bild von SD-, USB- oder SMB-Speicher. Es wird geprüft und sicher als PNG gespeichert.", "Elige una imagen PNG, JPEG, WebP o BMP del almacenamiento SD, USB o SMB. Se valida y guarda de forma segura como PNG.", "Scegli un'immagine PNG, JPEG, WebP o BMP da una memoria SD, USB o SMB. Verrà verificata e salvata in modo sicuro come PNG.", "Escolha uma imagem PNG, JPEG, WebP ou BMP do armazenamento SD, USB ou SMB. É validada e guardada em segurança como PNG."},
    {"Remove custom cover", "Supprimer la jaquette personnalisée", "Benutzerdefiniertes Cover entfernen", "Quitar carátula personalizada", "Rimuovi copertina personalizzata", "Remover capa personalizada"},
    {"Select local cover", "Sélectionner une jaquette locale", "Lokales Cover auswählen", "Seleccionar carátula local", "Seleziona copertina locale", "Selecionar capa local"},
    {"Cover imported", "Jaquette importée", "Cover importiert", "Carátula importada", "Copertina importata", "Capa importada"},
    {"Custom cover removed", "Jaquette personnalisée supprimée", "Benutzerdefiniertes Cover entfernt", "Carátula personalizada eliminada", "Copertina personalizzata rimossa", "Capa personalizada removida"},
    {"Downloading covers", "Téléchargement des jaquettes", "Cover werden heruntergeladen", "Descargando carátulas", "Download delle copertine", "A transferir capas"},
    {"Download cover", "Télécharger la jaquette", "Cover herunterladen", "Descargar carátula", "Scarica copertina", "Transferir capa"},
    {"Search", "Rechercher", "Suchen", "Buscar", "Cerca", "Pesquisar"},
    {"Search games", "Rechercher des jeux", "Spiele suchen", "Buscar juegos", "Cerca giochi", "Pesquisar jogos"},
    {"Favorites", "Favoris", "Favoriten", "Favoritos", "Preferiti", "Favoritos"},
    {"Collections", "Collections", "Sammlungen", "Colecciones", "Raccolte", "Coleções"},
    {"Manage collections", "Gérer les collections", "Sammlungen verwalten", "Gestionar colecciones", "Gestisci raccolte", "Gerir coleções"},
    {"All games", "Tous les jeux", "Alle Spiele", "Todos los juegos", "Tutti i giochi", "Todos os jogos"},
    {"Add to favorites", "Ajouter aux favoris", "Zu Favoriten hinzufügen", "Añadir a favoritos", "Aggiungi ai preferiti", "Adicionar aos favoritos"},
    {"Remove from favorites", "Retirer des favoris", "Aus Favoriten entfernen", "Quitar de favoritos", "Rimuovi dai preferiti", "Remover dos favoritos"},
    {"Create collection", "Créer une collection", "Sammlung erstellen", "Crear colección", "Crea raccolta", "Criar coleção"},
    {"Rename collection", "Renommer la collection", "Sammlung umbenennen", "Renombrar colección", "Rinomina raccolta", "Renomear coleção"},
    {"Delete collection", "Supprimer la collection", "Sammlung löschen", "Eliminar colección", "Elimina raccolta", "Eliminar coleção"},
    {"No games match this view", "Aucun jeu ne correspond à cette vue", "Keine Spiele entsprechen dieser Ansicht", "Ningún juego coincide con esta vista", "Nessun gioco corrisponde a questa vista", "Nenhum jogo corresponde a esta vista"},
    {"NO COVER", "AUCUNE JAQUETTE", "KEIN COVER", "SIN CARÁTULA", "NESSUNA COPERTINA", "SEM CAPA"},
    {"Loading game library...", "Chargement de la bibliothèque...", "Spielebibliothek wird geladen...", "Cargando la biblioteca...", "Caricamento della libreria...", "A carregar a biblioteca..."},
    {"The first page will appear as soon as it is ready.", "La première page s'affichera dès qu'elle sera prête.", "Die erste Seite erscheint, sobald sie bereit ist.", "La primera página aparecerá en cuanto esté lista.", "La prima pagina apparirà appena pronta.", "A primeira página aparecerá assim que estiver pronta."},
    {"Scanning game library...", "Analyse de la bibliothèque...", "Spielebibliothek wird durchsucht...", "Analizando la biblioteca...", "Scansione della libreria...", "A analisar a biblioteca..."},
    {"Reset", "Réinitialiser", "Zurücksetzen", "Restablecer", "Ripristina", "Repor"},
    {"Setting reset to default", "Paramètre réinitialisé à sa valeur par défaut", "Einstellung auf Standardwert zurückgesetzt", "Ajuste restablecido al valor predeterminado", "Impostazione ripristinata al valore predefinito", "Definição reposta para o valor predefinido"},
    {"Use global", "Utiliser le réglage global", "Globale Einstellung verwenden", "Usar ajuste global", "Usa impostazione globale", "Usar definição global"},
    {"Current:", "Actuel :", "Aktuell:", "Actual:", "Attuale:", "Atual:"},
    {"Close", "Fermer", "Schließen", "Cerrar", "Chiudi", "Fechar"},
    {"Choose", "Choisir", "Auswählen", "Elegir", "Scegli", "Escolher"},
    {"Change", "Modifier", "Ändern", "Cambiar", "Modifica", "Alterar"},
    {"Back", "Retour", "Zurück", "Atrás", "Indietro", "Voltar"},
    {"Yes", "Oui", "Ja", "Sí", "Sì", "Sim"},
    {"No", "Non", "Nein", "No", "No", "Não"},
    {"Exit", "Quitter", "Beenden", "Salir", "Esci", "Sair"},
    {"Exit Vita3K?", "Quitter Vita3K ?", "Vita3K beenden?", "¿Salir de Vita3K?", "Uscire da Vita3K?", "Sair do Vita3K?"},
    {"Return to the HOME Menu?", "Retourner au menu HOME ?", "Zum HOME-Menü zurückkehren?", "¿Volver al menú HOME?", "Tornare al menu HOME?", "Voltar ao Menu HOME?"},
    {"Active background operations will be cancelled safely.", "Les opérations en arrière-plan seront annulées en toute sécurité.", "Aktive Hintergrundvorgänge werden sicher abgebrochen.", "Las operaciones en segundo plano se cancelarán de forma segura.", "Le operazioni in background verranno annullate in sicurezza.", "As operações em segundo plano serão canceladas em segurança."},
    {"Closing Vita3K...", "Fermeture de Vita3K...", "Vita3K wird beendet...", "Cerrando Vita3K...", "Chiusura di Vita3K...", "A fechar o Vita3K..."},
    {"Finishing background operations safely.", "Finalisation sécurisée des opérations en arrière-plan.", "Hintergrundvorgänge werden sicher abgeschlossen.", "Finalizando de forma segura las operaciones en segundo plano.", "Completamento sicuro delle operazioni in background.", "A concluir as operações em segundo plano com segurança."},
    {"Applet mode installer", "Installation en mode applet", "Applet-Modus-Installer", "Instalador del modo applet", "Installazione in modalità applet", "Instalador do modo applet"},
    {"Vita3K is running in applet mode.", "Vita3K fonctionne en mode applet.", "Vita3K läuft im Applet-Modus.", "Vita3K se está ejecutando en modo applet.", "Vita3K è in esecuzione in modalità applet.", "O Vita3K está a ser executado no modo applet."},
    {"Applet mode has limited memory and is not suitable for emulation.", "Le mode applet dispose de peu de mémoire et ne convient pas à l'émulation.", "Der Applet-Modus hat wenig Speicher und eignet sich nicht zur Emulation.", "El modo applet tiene memoria limitada y no es adecuado para emular.", "La modalità applet ha memoria limitata e non è adatta all'emulazione.", "O modo applet tem memória limitada e não é adequado à emulação."},
    {"Install a HOME Menu shortcut to use full memory and normal performance.", "Installez un raccourci dans le menu HOME pour utiliser toute la mémoire et les performances normales.", "Installiere eine HOME-Menü-Verknüpfung für vollen Speicher und normale Leistung.", "Instala un acceso directo en el menú HOME para usar toda la memoria y el rendimiento normal.", "Installa un collegamento nel menu HOME per usare tutta la memoria e le prestazioni normali.", "Instale um atalho no Menu HOME para usar toda a memória e o desempenho normal."},
    {"Install to HOME Menu", "Installer dans le menu HOME", "Im HOME-Menü installieren", "Instalar en el menú HOME", "Installa nel menu HOME", "Instalar no Menu HOME"},
    {"Installing HOME Menu shortcut...", "Installation du raccourci du menu HOME...", "HOME-Menü-Verknüpfung wird installiert...", "Instalando el acceso directo del menú HOME...", "Installazione del collegamento nel menu HOME...", "A instalar o atalho do Menu HOME..."},
    {"Preparing Vita3K...", "Préparation de Vita3K...", "Vita3K wird vorbereitet...", "Preparando Vita3K...", "Preparazione di Vita3K...", "A preparar o Vita3K..."},
    {"Installed on the HOME Menu.", "Installé dans le menu HOME.", "Im HOME-Menü installiert.", "Instalado en el menú HOME.", "Installato nel menu HOME.", "Instalado no Menu HOME."},
    {"Installation failed", "Échec de l'installation", "Installation fehlgeschlagen", "Error de instalación", "Installazione non riuscita", "Falha na instalação"},
    {"Try again", "Réessayer", "Erneut versuchen", "Reintentar", "Riprova", "Tentar novamente"},
    {"Install", "Installer", "Installieren", "Instalar", "Installa", "Instalar"},
    {"Off", "Désactivé", "Aus", "Desactivado", "Disattivato", "Desativado"},
    {"On", "Activé", "Ein", "Activado", "Attivato", "Ativado"},
    {"Automatic", "Automatique", "Automatisch", "Automático", "Automatico", "Automático"},
    {"Manual", "Manuel", "Manuell", "Manual", "Manuale", "Manual"},
    {"Disabled", "Désactivé", "Deaktiviert", "Desactivado", "Disabilitato", "Desativado"},
    {"Compatibility", "Compatibilité", "Kompatibilität", "Compatibilidad", "Compatibilità", "Compatibilidade"},
    {"Performance", "Performances", "Leistung", "Rendimiento", "Prestazioni", "Desempenho"},
    {"Graphics", "Graphismes", "Grafik", "Gráficos", "Grafica", "Gráficos"},
    {"Display backend", "Moteur d'affichage", "Anzeige-Backend", "Motor de pantalla", "Backend di rendering", "Motor de apresentação"},
    {"Audio", "Audio", "Audio", "Audio", "Audio", "Áudio"},
    {"Controls", "Commandes", "Steuerung", "Controles", "Controlli", "Controlos"},
    {"Diagnostics", "Diagnostic", "Diagnose", "Diagnóstico", "Diagnostica", "Diagnóstico"},
    {"Network", "Réseau", "Netzwerk", "Red", "Rete", "Rede"},
    {"Interface", "Interface", "Oberfläche", "Interfaz", "Interfaccia", "Interface"},
    {"Modding", "Modding", "Modding", "Modding", "Modding", "Modding"},
    {"Vita system", "Système Vita", "Vita-System", "Sistema Vita", "Sistema Vita", "Sistema Vita"},
    {"System firmware", "Micrologiciel système", "System-Firmware", "Firmware del sistema", "Firmware di sistema", "Firmware do sistema"},
    {"Library layout", "Disposition de la bibliothèque", "Bibliothekslayout", "Diseño de la biblioteca", "Layout della libreria", "Disposição da biblioteca"},
    {"Launcher appearance", "Apparence du lanceur", "Launcher-Darstellung", "Apariencia del lanzador", "Aspetto del launcher", "Aspeto do launcher"},
    {"Launcher audio", "Audio du lanceur", "Launcher-Audio", "Audio del lanzador", "Audio del launcher", "Áudio do launcher"},
    {"Launcher language", "Langue du lanceur", "Launcher-Sprache", "Idioma del lanzador", "Lingua del launcher", "Idioma do launcher"},
    {"Launcher updates", "Mises à jour du lanceur", "Launcher-Aktualisierungen", "Actualizaciones del lanzador", "Aggiornamenti del launcher", "Atualizações do launcher"},
    {"Artwork service", "Service de jaquettes", "Cover-Dienst", "Servicio de carátulas", "Servizio copertine", "Serviço de capas"},
    {"Frame generation", "Génération d'images", "Frame-Generierung", "Generación de fotogramas", "Generazione fotogrammi", "Geração de fotogramas"},
    {"Frame generation quality", "Qualité de génération d'images", "Qualität der Frame-Generierung", "Calidad de generación de fotogramas", "Qualità della generazione fotogrammi", "Qualidade da geração de fotogramas"},
    {"Frame generation performance", "Performances de génération d'images", "Leistung der Frame-Generierung", "Rendimiento de generación de fotogramas", "Prestazioni della generazione fotogrammi", "Desempenho da geração de fotogramas"},
    {"Controls whether Vita system modules are loaded automatically or from a manual list. Automatic is recommended for most games.", "Détermine si les modules système Vita sont chargés automatiquement ou depuis une liste manuelle. Le mode automatique est recommandé pour la plupart des jeux.", "Legt fest, ob Vita-Systemmodule automatisch oder aus einer manuellen Liste geladen werden. Automatisch wird für die meisten Spiele empfohlen.", "Define si los módulos del sistema Vita se cargan automáticamente o desde una lista manual. Se recomienda Automático para la mayoría de juegos.", "Stabilisce se i moduli di sistema Vita vengono caricati automaticamente o da un elenco manuale. Automatico è consigliato per la maggior parte dei giochi.", "Define se os módulos do sistema Vita são carregados automaticamente ou através de uma lista manual. Automático é recomendado para a maioria dos jogos."},
    {"Enables Vita3K CPU optimisations. Disable only when diagnosing a title-specific CPU emulation problem.", "Active les optimisations CPU de Vita3K. Désactivez-les uniquement pour diagnostiquer un problème d'émulation CPU propre à un jeu.", "Aktiviert Vita3K-CPU-Optimierungen. Nur zur Diagnose eines spielspezifischen CPU-Emulationsproblems deaktivieren.", "Activa las optimizaciones de CPU de Vita3K. Desactívalas solo al diagnosticar un problema de emulación de CPU específico de un juego.", "Abilita le ottimizzazioni CPU di Vita3K. Disabilitale solo per diagnosticare un problema di emulazione CPU specifico di un gioco.", "Ativa as otimizações de CPU do Vita3K. Desative apenas para diagnosticar um problema de emulação de CPU específico de um jogo."},
    {"Prepares Vulkan LSFG 2x support for this game. Open the in-game quick menu with L + R + Plus to turn generated frames on or off. It does not increase emulation speed.", "Prépare la prise en charge de Vulkan LSFG 2x pour ce jeu. Ouvrez le menu rapide en jeu avec L + R + Plus pour activer ou désactiver les images générées. Cela n'accélère pas l'émulation.", "Bereitet Vulkan-LSFG-2x für dieses Spiel vor. Im Spiel mit L + R + Plus das Schnellmenü öffnen, um generierte Frames ein- oder auszuschalten. Die Emulationsgeschwindigkeit steigt dadurch nicht.", "Prepara la compatibilidad con Vulkan LSFG 2x para este juego. Abre el menú rápido con L + R + Plus para activar o desactivar los fotogramas generados. No aumenta la velocidad de emulación.", "Prepara il supporto Vulkan LSFG 2x per questo gioco. Apri il menu rapido in gioco con L + R + Plus per attivare o disattivare i fotogrammi generati. Non aumenta la velocità di emulazione.", "Prepara o suporte Vulkan LSFG 2x para este jogo. Abra o menu rápido no jogo com L + R + Plus para ativar ou desativar os fotogramas gerados. Não aumenta a velocidade da emulação."},
    {"Sets the optical-flow resolution. Quarter is recommended on Switch; Half can improve motion detail but costs more GPU time and memory.", "Règle la résolution du flux optique. Quarter est recommandé sur Switch ; Half peut améliorer les détails en mouvement, mais utilise plus de temps GPU et de mémoire.", "Legt die Auflösung des optischen Flusses fest. Quarter wird auf Switch empfohlen; Half kann Bewegungsdetails verbessern, benötigt aber mehr GPU-Zeit und Speicher.", "Ajusta la resolución del flujo óptico. Se recomienda Quarter en Switch; Half puede mejorar el detalle en movimiento, pero consume más GPU y memoria.", "Imposta la risoluzione del flusso ottico. Quarter è consigliato su Switch; Half può migliorare i dettagli in movimento ma richiede più GPU e memoria.", "Define a resolução do fluxo ótico. Quarter é recomendado na Switch; Half pode melhorar o detalhe em movimento, mas usa mais GPU e memória."},
    {"Uses LSFG's lighter performance-oriented path. Disable it only when image quality matters more than GPU headroom.", "Utilise le chemin LSFG allégé axé sur les performances. Désactivez-le uniquement si la qualité d'image est prioritaire sur la marge GPU.", "Nutzt den leichteren, leistungsorientierten LSFG-Pfad. Nur deaktivieren, wenn Bildqualität wichtiger als GPU-Reserve ist.", "Usa la ruta ligera de LSFG orientada al rendimiento. Desactívala solo si la calidad de imagen importa más que el margen de GPU.", "Usa il percorso LSFG più leggero orientato alle prestazioni. Disabilitalo solo se la qualità dell'immagine conta più del margine GPU.", "Usa o caminho LSFG mais leve, orientado ao desempenho. Desative apenas se a qualidade de imagem for mais importante do que a margem da GPU."},
    {"Chooses the Switch renderer. Vulkan (NVK) is recommended and supports LSFG. OpenGL uses native NVC0, while Zink runs OpenGL on NVK as an additional compatibility path.", "Choisit le moteur de rendu de la Switch. Vulkan (NVK) est recommandé et prend en charge LSFG. OpenGL utilise le pilote NVC0 natif, tandis que Zink exécute OpenGL sur NVK comme voie de compatibilité supplémentaire.", "Wählt den Switch-Renderer. Vulkan (NVK) wird empfohlen und unterstützt LSFG. OpenGL verwendet das native NVC0, während Zink OpenGL über NVK als zusätzlichen Kompatibilitätspfad ausführt.", "Elige el renderizador de Switch. Vulkan (NVK) es el recomendado y admite LSFG. OpenGL usa NVC0 nativo, mientras Zink ejecuta OpenGL sobre NVK como ruta de compatibilidad adicional.", "Sceglie il renderer di Switch. Vulkan (NVK) è consigliato e supporta LSFG. OpenGL usa NVC0 nativo, mentre Zink esegue OpenGL su NVK come percorso di compatibilità aggiuntivo.", "Seleciona o renderizador da Switch. Vulkan (NVK) é recomendado e suporta LSFG. OpenGL usa NVC0 nativo, enquanto o Zink executa OpenGL sobre NVK como via de compatibilidade adicional."},
    {"Controls renderer selection, graphics quality, scaling, caches, and performance diagnostics.", "Contrôle le choix du moteur de rendu, la qualité graphique, la mise à l'échelle, les caches et les diagnostics de performances.", "Steuert Renderer-Auswahl, Grafikqualität, Skalierung, Caches und Leistungsdiagnose.", "Controla la selección del renderizador, la calidad gráfica, el escalado, las cachés y los diagnósticos de rendimiento.", "Controlla la scelta del renderer, la qualità grafica, il ridimensionamento, le cache e la diagnostica delle prestazioni.", "Controla a seleção do renderizador, a qualidade gráfica, a escala, as caches e os diagnósticos de desempenho."},
    {"Scales the Vita render resolution. Higher values sharpen the image but increase GPU and memory cost.", "Met à l'échelle la résolution de rendu Vita. Une valeur élevée affine l'image, mais augmente le coût GPU et mémoire.", "Skaliert die Vita-Renderauflösung. Höhere Werte schärfen das Bild, erhöhen aber GPU- und Speicherbedarf.", "Escala la resolución de renderizado de Vita. Valores altos hacen la imagen más nítida, pero aumentan el uso de GPU y memoria.", "Scala la risoluzione di rendering Vita. Valori più alti rendono l'immagine più nitida, ma aumentano il costo GPU e memoria.", "Dimensiona a resolução de renderização Vita. Valores maiores tornam a imagem mais nítida, mas aumentam o uso de GPU e memória."},
    {"Selects how Vita GPU memory is mirrored for Vulkan. Double buffer is the Switch default and the only mapped mode this GPU handles well. Disabled turns mapping off entirely, which some games need.", "Choisit comment la mémoire GPU Vita est répliquée pour Vulkan. Double buffer est le réglage par défaut sur Switch et le seul mode mappé que ce GPU gère bien. Désactivé coupe entièrement le mappage, ce dont certains jeux ont besoin.", "Wählt, wie Vita-GPU-Speicher für Vulkan gespiegelt wird. Double buffer ist der Switch-Standard und der einzige gemappte Modus, den diese GPU gut beherrscht. Deaktiviert schaltet das Mapping ganz ab, was manche Spiele benötigen.", "Elige cómo se refleja la memoria GPU de Vita para Vulkan. Double buffer es el valor predeterminado en Switch y el único modo mapeado que esta GPU maneja bien. Desactivado desactiva el mapeo por completo, algo que algunos juegos necesitan.", "Sceglie come viene replicata la memoria GPU Vita per Vulkan. Double buffer è il valore predefinito su Switch e l’unica modalità mappata che questa GPU gestisce bene. Disabilitato disattiva del tutto la mappatura, cosa che alcuni giochi richiedono.", "Seleciona como a memória GPU da Vita é espelhada para Vulkan. Double buffer é o padrão na Switch e o único modo mapeado que esta GPU lida bem. Desativado desliga o mapeamento por completo, o que alguns jogos precisam."},
    {"Uses more accurate GPU behavior for games that render incorrectly, at a possible performance cost.", "Utilise un comportement GPU plus précis pour les jeux dont le rendu est incorrect, avec un coût possible en performances.", "Nutzt genaueres GPU-Verhalten für Spiele mit fehlerhafter Darstellung, möglicherweise auf Kosten der Leistung.", "Usa un comportamiento de GPU más preciso para juegos con renderizado incorrecto, con posible coste de rendimiento.", "Usa un comportamento GPU più accurato per i giochi con rendering errato, con un possibile costo in prestazioni.", "Usa um comportamento de GPU mais preciso para jogos com renderização incorreta, com possível custo de desempenho."},
    {"Chooses the final image scaling filter. Nearest is sharp, Bilinear is inexpensive, and advanced filters cost more GPU time.", "Choisit le filtre final de mise à l'échelle. Nearest est net, Bilinear est peu coûteux et les filtres avancés utilisent plus de GPU.", "Wählt den finalen Bildskalierungsfilter. Nearest ist scharf, Bilinear ist günstig und erweiterte Filter benötigen mehr GPU-Zeit.", "Elige el filtro final de escalado. Nearest es nítido, Bilinear es ligero y los filtros avanzados consumen más GPU.", "Sceglie il filtro finale di ridimensionamento. Nearest è nitido, Bilinear è leggero e i filtri avanzati richiedono più GPU.", "Seleciona o filtro final de escala. Nearest é nítido, Bilinear é leve e os filtros avançados usam mais GPU."},
    {"Synchronizes presentation to the display refresh to avoid visible tearing. On drivers that expose only FIFO presentation this remains enabled by the driver.", "Synchronise l'affichage avec le rafraîchissement de l'écran pour éviter le tearing. Sur les pilotes proposant uniquement FIFO, le pilote le maintient activé.", "Synchronisiert die Ausgabe mit der Bildwiederholrate, um Tearing zu vermeiden. Bei Treibern mit ausschließlich FIFO bleibt dies durch den Treiber aktiviert.", "Sincroniza la presentación con el refresco de pantalla para evitar tearing. En controladores que solo ofrecen FIFO, el controlador lo mantiene activado.", "Sincronizza la presentazione con il refresh dello schermo per evitare tearing. Sui driver che offrono solo FIFO resta attivo tramite il driver.", "Sincroniza a apresentação com a atualização do ecrã para evitar tearing. Em drivers que apenas expõem FIFO, permanece ativado pelo driver."},
    {"Improves texture clarity at oblique angles. Higher levels use additional GPU bandwidth.", "Améliore la netteté des textures sous des angles obliques. Les niveaux élevés utilisent davantage de bande passante GPU.", "Verbessert die Texturschärfe in schrägen Winkeln. Höhere Stufen benötigen zusätzliche GPU-Bandbreite.", "Mejora la claridad de las texturas en ángulos oblicuos. Los niveles altos usan más ancho de banda de GPU.", "Migliora la nitidezza delle texture ad angoli obliqui. Livelli più alti usano più banda GPU.", "Melhora a nitidez das texturas em ângulos oblíquos. Níveis maiores usam mais largura de banda da GPU."},
    {"Skips expensive surface synchronization. The Switch default favors performance, but a game with missing or stale graphics may need it enabled.", "Ignore la synchronisation coûteuse des surfaces. Le réglage Switch privilégie les performances, mais un jeu avec des graphismes absents ou figés peut nécessiter son activation.", "Überspringt aufwendige Oberflächensynchronisierung. Der Switch-Standard bevorzugt Leistung; bei fehlender oder veralteter Grafik kann sie nötig sein.", "Omite la costosa sincronización de superficies. El valor de Switch favorece el rendimiento, pero un juego con gráficos ausentes o desactualizados puede necesitarla.", "Salta la costosa sincronizzazione delle superfici. Il valore Switch favorisce le prestazioni, ma un gioco con grafica mancante o non aggiornata può richiederla.", "Ignora a sincronização dispendiosa de superfícies. O padrão da Switch favorece o desempenho, mas um jogo com gráficos ausentes ou desatualizados pode precisar dela."},
    {"Caches decoded and uploaded textures. Disabling is intended only for graphics troubleshooting.", "Met en cache les textures décodées et envoyées. La désactivation sert uniquement au diagnostic graphique.", "Speichert dekodierte und hochgeladene Texturen zwischen. Deaktivieren ist nur zur Grafikdiagnose gedacht.", "Guarda en caché las texturas decodificadas y subidas. Desactívalo solo para diagnosticar problemas gráficos.", "Memorizza le texture decodificate e caricate. La disattivazione serve solo alla diagnostica grafica.", "Coloca em cache as texturas descodificadas e enviadas. Desative apenas para diagnóstico gráfico."},
    {"Compiles Vulkan pipelines asynchronously to reduce stalls. Newly encountered effects can appear briefly after compilation.", "Compile les pipelines Vulkan de façon asynchrone pour réduire les blocages. Les nouveaux effets peuvent apparaître brièvement après leur compilation.", "Kompiliert Vulkan-Pipelines asynchron, um Stocken zu verringern. Neue Effekte können kurz verzögert erscheinen.", "Compila las canalizaciones Vulkan de forma asíncrona para reducir pausas. Los efectos nuevos pueden aparecer brevemente tras compilarse.", "Compila le pipeline Vulkan in modo asincrono per ridurre i blocchi. Gli effetti nuovi possono apparire con un breve ritardo.", "Compila pipelines Vulkan de forma assíncrona para reduzir pausas. Efeitos novos podem aparecer com um pequeno atraso."},
    {"Shows Vita3K's shader compilation indicator while new graphics pipelines are prepared.", "Affiche l'indicateur de compilation des shaders de Vita3K pendant la préparation de nouveaux pipelines graphiques.", "Zeigt Vita3Ks Shader-Kompilierungsanzeige, während neue Grafik-Pipelines vorbereitet werden.", "Muestra el indicador de compilación de shaders de Vita3K mientras se preparan nuevas canalizaciones gráficas.", "Mostra l'indicatore di compilazione shader di Vita3K durante la preparazione di nuove pipeline grafiche.", "Mostra o indicador de compilação de shaders do Vita3K enquanto novas pipelines gráficas são preparadas."},
    {"Reuses compiled shaders between sessions to reduce later stutter. Clearing a broken cache is available from the game menu.", "Réutilise les shaders compilés entre les sessions pour réduire les saccades. Un cache défectueux peut être effacé depuis le menu du jeu.", "Verwendet kompilierte Shader sitzungsübergreifend, um späteres Ruckeln zu verringern. Ein defekter Cache kann im Spielmenü gelöscht werden.", "Reutiliza shaders compilados entre sesiones para reducir tirones posteriores. Se puede borrar una caché dañada desde el menú del juego.", "Riutilizza gli shader compilati tra le sessioni per ridurre gli scatti. Una cache danneggiata può essere cancellata dal menu del gioco.", "Reutiliza shaders compilados entre sessões para reduzir engasgos posteriores. Uma cache danificada pode ser limpa no menu do jogo."},
    {"Loads replacement textures from Vita3K's texture import directory.", "Charge les textures de remplacement depuis le dossier d'importation de Vita3K.", "Lädt Ersatztexturen aus Vita3Ks Textur-Importordner.", "Carga texturas de reemplazo desde la carpeta de importación de Vita3K.", "Carica texture sostitutive dalla cartella di importazione di Vita3K.", "Carrega texturas de substituição a partir da pasta de importação do Vita3K."},
    {"Dumps textures used by the game for replacement or inspection. This increases SD-card I/O.", "Exporte les textures utilisées par le jeu pour les remplacer ou les inspecter. Cela augmente les accès à la carte SD.", "Speichert vom Spiel verwendete Texturen zum Ersetzen oder Prüfen. Dies erhöht die SD-Karten-I/O.", "Vuelca las texturas usadas por el juego para reemplazarlas o revisarlas. Esto aumenta la E/S de la tarjeta SD.", "Esporta le texture usate dal gioco per sostituzione o analisi. Aumenta l'I/O della scheda SD.", "Exporta as texturas usadas pelo jogo para substituição ou inspeção. Isto aumenta a E/S do cartão SD."},
    {"Writes exported textures as PNG rather than their raw format. PNG is convenient but slower to encode.", "Écrit les textures exportées en PNG plutôt qu'au format brut. Le PNG est pratique, mais plus lent à encoder.", "Schreibt exportierte Texturen als PNG statt im Rohformat. PNG ist praktisch, aber langsamer zu kodieren.", "Guarda las texturas exportadas como PNG en vez de formato sin procesar. PNG es práctico, pero tarda más en codificarse.", "Salva le texture esportate come PNG invece che in formato grezzo. PNG è comodo ma più lento da codificare.", "Guarda as texturas exportadas como PNG em vez do formato bruto. PNG é prático, mas mais lento a codificar."},
    {"Enables Vita3K's experimental frame-rate hack. It can alter game speed or timing in unsupported titles.", "Active le hack expérimental de fréquence d'images de Vita3K. Il peut modifier la vitesse ou le timing des jeux non pris en charge.", "Aktiviert Vita3Ks experimentellen Bildraten-Hack. Bei nicht unterstützten Spielen kann er Geschwindigkeit oder Timing verändern.", "Activa el hack experimental de tasa de fotogramas de Vita3K. Puede alterar la velocidad o los tiempos de juegos no compatibles.", "Abilita l'hack sperimentale del frame rate di Vita3K. Può alterare velocità o temporizzazione nei giochi non supportati.", "Ativa o hack experimental de taxa de fotogramas do Vita3K. Pode alterar a velocidade ou temporização de jogos não suportados."},
    {"Shows the emulator performance overlay while a game is running.", "Affiche l'overlay de performances de l'émulateur pendant l'exécution d'un jeu.", "Zeigt während des Spiels das Leistungs-Overlay des Emulators.", "Muestra la superposición de rendimiento del emulador durante el juego.", "Mostra l'overlay delle prestazioni dell'emulatore durante il gioco.", "Mostra a sobreposição de desempenho do emulador durante o jogo."},
    {"Controls how much timing and performance information the overlay displays.", "Règle la quantité d'informations de timing et de performances affichées par l'overlay.", "Steuert, wie viele Timing- und Leistungsdaten das Overlay anzeigt.", "Controla cuánta información de tiempos y rendimiento muestra la superposición.", "Controlla quante informazioni su tempi e prestazioni mostra l'overlay.", "Controla a quantidade de informação de temporização e desempenho mostrada pela sobreposição."},
    {"Places the performance overlay in a screen corner or along the top or bottom edge.", "Place l'overlay de performances dans un coin de l'écran ou le long du bord supérieur ou inférieur.", "Platziert das Leistungs-Overlay in einer Bildschirmecke oder am oberen bzw. unteren Rand.", "Coloca la superposición de rendimiento en una esquina o en el borde superior o inferior.", "Posiziona l'overlay delle prestazioni in un angolo o lungo il bordo superiore o inferiore.", "Posiciona a sobreposição de desempenho num canto ou junto à margem superior ou inferior."},
    {"Sets Vita3K's output volume before it reaches the Switch system volume.", "Règle le volume de sortie de Vita3K avant le volume système de la Switch.", "Legt Vita3Ks Ausgangslautstärke vor der Switch-Systemlautstärke fest.", "Ajusta el volumen de salida de Vita3K antes del volumen del sistema Switch.", "Imposta il volume di uscita di Vita3K prima del volume di sistema di Switch.", "Define o volume de saída do Vita3K antes do volume do sistema da Switch."},
    {"Enables emulation of the Vita NGS audio engine used by many games.", "Active l'émulation du moteur audio NGS de la Vita utilisé par de nombreux jeux.", "Aktiviert die Emulation der von vielen Spielen genutzten Vita-NGS-Audioengine.", "Activa la emulación del motor de audio NGS de Vita usado por muchos juegos.", "Abilita l'emulazione del motore audio NGS di Vita usato da molti giochi.", "Ativa a emulação do motor de áudio NGS da Vita usado por muitos jogos."},
    {"Sets the language reported to Vita software. Games that support it may choose matching text and audio.", "Règle la langue indiquée aux logiciels Vita. Les jeux compatibles peuvent choisir les textes et l'audio correspondants.", "Legt die an Vita-Software gemeldete Sprache fest. Unterstützte Spiele können passende Texte und Audiospuren wählen.", "Ajusta el idioma indicado al software de Vita. Los juegos compatibles pueden usar texto y audio correspondientes.", "Imposta la lingua comunicata al software Vita. I giochi compatibili possono scegliere testo e audio corrispondenti.", "Define o idioma comunicado ao software Vita. Jogos compatíveis podem escolher texto e áudio correspondentes."},
    {"Chooses whether Cross or Circle is reported as the Vita system confirmation button.", "Choisit si Croix ou Cercle est indiqué comme bouton de confirmation du système Vita.", "Wählt, ob Kreuz oder Kreis als Bestätigungstaste des Vita-Systems gemeldet wird.", "Elige si Cruz o Círculo se usa como botón de confirmación del sistema Vita.", "Sceglie se Croce o Cerchio viene indicato come tasto di conferma del sistema Vita.", "Seleciona se Cruz ou Círculo é comunicado como botão de confirmação do sistema Vita."},
    {"Sets the date format exposed through Vita system parameters.", "Règle le format de date exposé par les paramètres système Vita.", "Legt das über Vita-Systemparameter bereitgestellte Datumsformat fest.", "Ajusta el formato de fecha expuesto mediante los parámetros del sistema Vita.", "Imposta il formato data esposto tramite i parametri di sistema Vita.", "Define o formato de data exposto pelos parâmetros do sistema Vita."},
    {"Sets the 12-hour or 24-hour clock format exposed to games.", "Règle le format d'horloge 12 ou 24 heures exposé aux jeux.", "Legt das für Spiele bereitgestellte 12- oder 24-Stunden-Zeitformat fest.", "Ajusta el formato de reloj de 12 o 24 horas expuesto a los juegos.", "Imposta il formato orario a 12 o 24 ore esposto ai giochi.", "Define o formato de relógio de 12 ou 24 horas exposto aos jogos."},
    {"Reports a PlayStation TV environment to software. Some games change controls or block unsupported modes.", "Indique aux logiciels un environnement PlayStation TV. Certains jeux modifient leurs commandes ou bloquent les modes non pris en charge.", "Meldet der Software eine PlayStation-TV-Umgebung. Manche Spiele ändern die Steuerung oder sperren nicht unterstützte Modi.", "Informa al software de un entorno PlayStation TV. Algunos juegos cambian los controles o bloquean modos no compatibles.", "Comunica al software un ambiente PlayStation TV. Alcuni giochi cambiano i controlli o bloccano modalità non supportate.", "Comunica ao software um ambiente PlayStation TV. Alguns jogos alteram os controlos ou bloqueiam modos não suportados."},
    {"Allows Vita software to use Vita3K's HTTP networking implementation.", "Autorise les logiciels Vita à utiliser l'implémentation réseau HTTP de Vita3K.", "Erlaubt Vita-Software, Vita3Ks HTTP-Netzwerkimplementierung zu nutzen.", "Permite que el software de Vita use la implementación de red HTTP de Vita3K.", "Consente al software Vita di usare l'implementazione di rete HTTP di Vita3K.", "Permite ao software Vita usar a implementação de rede HTTP do Vita3K."},
    {"Sets how many polling attempts an HTTP operation receives before timing out.", "Règle le nombre de tentatives d'interrogation d'une opération HTTP avant expiration.", "Legt die Anzahl der HTTP-Abfrageversuche bis zum Timeout fest.", "Ajusta cuántos intentos de consulta tiene una operación HTTP antes de agotar el tiempo.", "Imposta il numero di tentativi di interrogazione HTTP prima del timeout.", "Define o número de tentativas de consulta de uma operação HTTP antes de expirar."},
    {"Sets the delay between HTTP timeout polling attempts.", "Règle le délai entre les tentatives d'interrogation du délai HTTP.", "Legt die Pause zwischen HTTP-Timeout-Abfragen fest.", "Ajusta el intervalo entre intentos de consulta de tiempo de espera HTTP.", "Imposta l'intervallo tra i tentativi di timeout HTTP.", "Define o intervalo entre tentativas de timeout HTTP."},
    {"Sets how many times Vita3K checks for the end of an HTTP response.", "Règle le nombre de vérifications de fin de réponse HTTP effectuées par Vita3K.", "Legt fest, wie oft Vita3K das Ende einer HTTP-Antwort prüft.", "Ajusta cuántas veces Vita3K comprueba el final de una respuesta HTTP.", "Imposta quante volte Vita3K controlla la fine di una risposta HTTP.", "Define quantas vezes o Vita3K verifica o fim de uma resposta HTTP."},
    {"Sets the delay between HTTP response-end checks.", "Règle le délai entre les vérifications de fin de réponse HTTP.", "Legt die Pause zwischen Prüfungen auf das Ende einer HTTP-Antwort fest.", "Ajusta el intervalo entre comprobaciones del final de una respuesta HTTP.", "Imposta l'intervallo tra i controlli di fine risposta HTTP.", "Define o intervalo entre verificações do fim de uma resposta HTTP."},
    {"Reports a signed-in PSN state to games. It does not sign the console into PlayStation Network.", "Indique aux jeux une connexion PSN active. Cela ne connecte pas la console au PlayStation Network.", "Meldet Spielen einen angemeldeten PSN-Status. Die Konsole wird dadurch nicht beim PlayStation Network angemeldet.", "Informa a los juegos de una sesión PSN iniciada. No conecta la consola a PlayStation Network.", "Comunica ai giochi uno stato PSN connesso. Non accede al PlayStation Network dalla console.", "Comunica aos jogos um estado PSN com sessão iniciada. Não liga a consola à PlayStation Network."},
    {"Selects the local address index used by Vita ad-hoc networking.", "Sélectionne l'index d'adresse locale utilisé par le réseau ad hoc Vita.", "Wählt den lokalen Adressindex für Vita-Ad-hoc-Netzwerke.", "Selecciona el índice de dirección local usado por la red ad hoc de Vita.", "Seleziona l'indice dell'indirizzo locale usato dalla rete ad hoc Vita.", "Seleciona o índice do endereço local usado pela rede ad hoc Vita."},
    {"Disables Vita motion-sensor input derived from the active Switch controller.", "Désactive les capteurs de mouvement Vita issus de la manette Switch active.", "Deaktiviert Vita-Bewegungssensordaten vom aktiven Switch-Controller.", "Desactiva la entrada de movimiento de Vita procedente del mando Switch activo.", "Disabilita l'input dei sensori di movimento Vita derivato dal controller Switch attivo.", "Desativa a entrada dos sensores de movimento Vita proveniente do comando Switch ativo."},
    {"Scales analog stick movement before it is sent to the emulated Vita.", "Met à l'échelle le mouvement du stick analogique avant son envoi à la Vita émulée.", "Skaliert Analogstick-Bewegungen, bevor sie an die emulierte Vita gesendet werden.", "Escala el movimiento del stick analógico antes de enviarlo a la Vita emulada.", "Scala il movimento dello stick analogico prima di inviarlo alla Vita emulata.", "Dimensiona o movimento do analógico antes de o enviar à Vita emulada."},
    {"Ignores small stick movements to reduce drift. Too high a value reduces fine control.", "Ignore les petits mouvements du stick pour réduire le drift. Une valeur trop élevée réduit la précision.", "Ignoriert kleine Stickbewegungen gegen Drift. Ein zu hoher Wert verringert die Feinsteuerung.", "Ignora pequeños movimientos del stick para reducir el drift. Un valor alto reduce el control preciso.", "Ignora piccoli movimenti dello stick per ridurre il drift. Un valore troppo alto riduce la precisione.", "Ignora pequenos movimentos do analógico para reduzir drift. Um valor demasiado alto reduz o controlo preciso."},
    {"Chooses the shoulder-button modifier used with the touchscreen to emulate the Vita rear touch panel.", "Choisit la gâchette utilisée avec l'écran tactile pour émuler le pavé tactile arrière de la Vita.", "Wählt die Schultertasten-Umschaltung, die zusammen mit dem Touchscreen das rückseitige Vita-Touchpad emuliert.", "Elige el modificador de hombro usado con la pantalla táctil para emular el panel táctil trasero de Vita.", "Sceglie il modificatore dorsale usato con il touchscreen per emulare il touch posteriore Vita.", "Seleciona o modificador de ombro usado com o ecrã tátil para emular o painel tátil traseiro da Vita."},
    {"Chooses which Vita face button is produced by Nintendo Switch A.", "Choisit le bouton Vita produit par le bouton A de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch A erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch A.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch A.", "Seleciona qual botão frontal Vita é produzido pelo A da Nintendo Switch."},
    {"Chooses which Vita face button is produced by Nintendo Switch B.", "Choisit le bouton Vita produit par le bouton B de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch B erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch B.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch B.", "Seleciona qual botão frontal Vita é produzido pelo B da Nintendo Switch."},
    {"Chooses which Vita face button is produced by Nintendo Switch X.", "Choisit le bouton Vita produit par le bouton X de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch X erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch X.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch X.", "Seleciona qual botão frontal Vita é produzido pelo X da Nintendo Switch."},
    {"Chooses which Vita face button is produced by Nintendo Switch Y.", "Choisit le bouton Vita produit par le bouton Y de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch Y erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch Y.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch Y.", "Seleciona qual botão frontal Vita é produzido pelo Y da Nintendo Switch."},
    {"Selects the launcher background and color treatment. XMB, Bubbles, and Glow include optional animation.", "Sélectionne l'arrière-plan et les couleurs du lanceur. XMB, Bubbles et Glow proposent des animations facultatives.", "Wählt Hintergrund und Farbgestaltung des Launchers. XMB, Bubbles und Glow bieten optionale Animationen.", "Selecciona el fondo y los colores del lanzador. XMB, Bubbles y Glow incluyen animación opcional.", "Seleziona lo sfondo e i colori del launcher. XMB, Bubbles e Glow includono animazioni opzionali.", "Seleciona o fundo e as cores do launcher. XMB, Bubbles e Glow incluem animação opcional."},
    {"Rotates the complete launcher interface and touch coordinates in 90-degree steps.", "Fait pivoter toute l'interface du lanceur et les coordonnées tactiles par pas de 90 degrés.", "Dreht die gesamte Launcher-Oberfläche und Touch-Koordinaten in 90-Grad-Schritten.", "Gira toda la interfaz del lanzador y las coordenadas táctiles en pasos de 90 grados.", "Ruota l'intera interfaccia del launcher e le coordinate touch a passi di 90 gradi.", "Roda toda a interface do launcher e as coordenadas táteis em incrementos de 90 graus."},
    {"Sets the number of game covers shown across each library page.", "Règle le nombre de jaquettes affichées horizontalement sur chaque page de la bibliothèque.", "Legt die Anzahl der Spielcover pro Zeile jeder Bibliotheksseite fest.", "Ajusta el número de carátulas mostradas horizontalmente en cada página de la biblioteca.", "Imposta il numero di copertine mostrate orizzontalmente in ogni pagina della libreria.", "Define o número de capas mostradas horizontalmente em cada página da biblioteca."},
    {"Sets the number of cover rows shown on each library page.", "Règle le nombre de lignes de jaquettes affichées sur chaque page de la bibliothèque.", "Legt die Anzahl der Cover-Zeilen pro Bibliotheksseite fest.", "Ajusta el número de filas de carátulas en cada página de la biblioteca.", "Imposta il numero di righe di copertine in ogni pagina della libreria.", "Define o número de linhas de capas em cada página da biblioteca."},
    {"Shows or hides game names below cover artwork.", "Affiche ou masque le nom des jeux sous leur jaquette.", "Zeigt oder verbirgt Spielnamen unter dem Cover.", "Muestra u oculta los nombres de los juegos bajo las carátulas.", "Mostra o nasconde i nomi dei giochi sotto le copertine.", "Mostra ou oculta os nomes dos jogos sob as capas."},
    {"Shows or hides the region flag in the top-left corner of each game cover.", "Affiche ou masque le drapeau de région dans le coin supérieur gauche de chaque jaquette.", "Blendet die Regionsflagge oben links auf jedem Spielcover ein oder aus.", "Muestra u oculta la bandera de región en la esquina superior izquierda de cada carátula.", "Mostra o nasconde la bandiera regionale nell'angolo superiore sinistro di ogni copertina.", "Mostra ou oculta a bandeira de região no canto superior esquerdo de cada capa."},
    {"Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed.", "Affiche ou masque l'indicateur carré sur les jeux ayant des paramètres par jeu. Les paramètres eux-mêmes ne sont pas modifiés.", "Blendet die quadratische Markierung bei Spielen mit spielspezifischen Einstellungen ein oder aus. Die Einstellungen selbst werden nicht geändert.", "Muestra u oculta el indicador cuadrado en los juegos con ajustes por juego. Los ajustes no se modifican.", "Mostra o nasconde l'indicatore quadrato sui giochi con impostazioni specifiche. Le impostazioni non vengono modificate.", "Mostra ou oculta o indicador quadrado nos jogos com definições por jogo. As definições não são alteradas."},
    {"Enables animated backgrounds, fades, highlight easing, and cover transitions.", "Active les arrière-plans animés, fondus, déplacements fluides de sélection et transitions de jaquettes.", "Aktiviert animierte Hintergründe, Überblendungen, weiche Auswahlbewegung und Cover-Übergänge.", "Activa fondos animados, fundidos, movimiento suave del resaltado y transiciones de carátulas.", "Abilita sfondi animati, dissolvenze, movimento fluido della selezione e transizioni delle copertine.", "Ativa fundos animados, desvanecimentos, movimento suave da seleção e transições das capas."},
    {"Enables navigation, confirmation, and back sound effects in the launcher.", "Active les sons de navigation, de confirmation et de retour du lanceur.", "Aktiviert Navigations-, Bestätigungs- und Zurück-Sounds im Launcher.", "Activa los sonidos de navegación, confirmación y retroceso del lanzador.", "Abilita i suoni di navigazione, conferma e ritorno nel launcher.", "Ativa os sons de navegação, confirmação e retrocesso no launcher."},
    {"Checks the official Vita3K-nx GitHub releases in the background when the library opens. Direct HOME shortcut launches skip the check.", "Vérifie en arrière-plan les versions officielles de Vita3K-nx sur GitHub à l'ouverture de la bibliothèque. Les raccourcis HOME directs ignorent cette vérification.", "Prüft beim Öffnen der Bibliothek im Hintergrund die offiziellen Vita3K-nx-GitHub-Versionen. Direkte HOME-Verknüpfungen überspringen die Prüfung.", "Busca en segundo plano versiones oficiales de Vita3K-nx en GitHub al abrir la biblioteca. Los accesos directos HOME omiten la comprobación.", "Controlla in background le versioni ufficiali Vita3K-nx su GitHub all'apertura della libreria. I collegamenti HOME diretti saltano il controllo.", "Verifica em segundo plano as versões oficiais do Vita3K-nx no GitHub ao abrir a biblioteca. Atalhos HOME diretos ignoram a verificação."},
    {"Changes only the SDL launcher's language. Vita system language is configured separately under System.", "Modifie uniquement la langue du lanceur SDL. La langue du système Vita se règle séparément dans Système.", "Ändert nur die Sprache des SDL-Launchers. Die Vita-Systemsprache wird separat unter System eingestellt.", "Cambia solo el idioma del lanzador SDL. El idioma del sistema Vita se configura por separado en Sistema.", "Cambia solo la lingua del launcher SDL. La lingua di sistema Vita si configura separatamente in Sistema.", "Altera apenas o idioma do launcher SDL. O idioma do sistema Vita é configurado separadamente em Sistema."},
    {"Sets the SteamGridDB API key used for cover and HOME shortcut artwork searches. Leave it empty to remove the key.", "Règle la clé API SteamGridDB utilisée pour rechercher les jaquettes et icônes des raccourcis HOME. Laissez vide pour supprimer la clé.", "Legt den SteamGridDB-API-Schlüssel für Cover- und HOME-Verknüpfungsgrafiken fest. Leer lassen, um den Schlüssel zu entfernen.", "Ajusta la clave API de SteamGridDB usada para buscar carátulas e imágenes de accesos HOME. Déjala vacía para borrar la clave.", "Imposta la chiave API SteamGridDB usata per cercare copertine e immagini dei collegamenti HOME. Lascia vuoto per rimuoverla.", "Define a chave API SteamGridDB usada para procurar capas e imagens de atalhos HOME. Deixe vazio para remover a chave."},
    {"System data / font", "Données système / police", "Systemdaten / Schriftart", "Datos del sistema / fuente", "Dati di sistema / carattere", "Dados do sistema / tipo de letra"},
    {"LSFG 2x (Vulkan only)", "LSFG 2x (Vulkan uniquement)", "LSFG 2x (nur Vulkan)", "LSFG 2x (solo Vulkan)", "LSFG 2x (solo Vulkan)", "LSFG 2x (apenas Vulkan)"},
    {"Flow resolution", "Résolution du flux", "Flussauflösung", "Resolución del flujo", "Risoluzione del flusso", "Resolução do fluxo"},
    {"Performance mode", "Mode performances", "Leistungsmodus", "Modo rendimiento", "Modalità prestazioni", "Modo de desempenho"},
    {"Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll"},
    {"Modules mode", "Mode des modules", "Modulmodus", "Modo de módulos", "Modalità moduli", "Modo de módulos"},
    {"CPU optimisations", "Optimisations CPU", "CPU-Optimierungen", "Optimizaciones de CPU", "Ottimizzazioni CPU", "Otimizações de CPU"},
    {"Renderer", "Moteur de rendu", "Renderer", "Renderizador", "Renderer", "Renderizador"},
    {"Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)"},
    {"OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)"},
    {"Zink (OpenGL on NVK)", "Zink (OpenGL sur NVK)", "Zink (OpenGL über NVK)", "Zink (OpenGL sobre NVK)", "Zink (OpenGL su NVK)", "Zink (OpenGL sobre NVK)"},
    {"Vulkan only", "Vulkan uniquement", "Nur Vulkan", "Solo Vulkan", "Solo Vulkan", "Apenas Vulkan"},
    {"Resolution scale", "Échelle de résolution", "Auflösungsskalierung", "Escala de resolución", "Scala risoluzione", "Escala de resolução"},
    {"Memory mapping", "Mappage mémoire", "Speicherabbildung", "Mapeo de memoria", "Mappatura memoria", "Mapeamento de memória"},
    {"High accuracy", "Haute précision", "Hohe Genauigkeit", "Alta precisión", "Alta precisione", "Alta precisão"},
    {"Screen filter", "Filtre d'écran", "Bildschirmfilter", "Filtro de pantalla", "Filtro schermo", "Filtro de ecrã"},
    {"VSync", "VSync", "VSync", "VSync", "VSync", "VSync"},
    {"Anisotropic filtering", "Filtrage anisotrope", "Anisotrope Filterung", "Filtrado anisotrópico", "Filtro anisotropico", "Filtragem anisotrópica"},
    {"Disable surface sync", "Désactiver la synchronisation des surfaces", "Oberflächensynchronisierung deaktivieren", "Desactivar sincronización de superficies", "Disabilita sincronizzazione superfici", "Desativar sincronização de superfícies"},
    {"Texture cache", "Cache des textures", "Textur-Cache", "Caché de texturas", "Cache texture", "Cache de texturas"},
    {"Async pipeline compile", "Compilation asynchrone des pipelines", "Asynchrone Pipeline-Kompilierung", "Compilación asíncrona de canalizaciones", "Compilazione asincrona pipeline", "Compilação assíncrona de pipelines"},
    {"Show compiling shaders", "Afficher la compilation des shaders", "Shader-Kompilierung anzeigen", "Mostrar compilación de shaders", "Mostra compilazione shader", "Mostrar compilação de shaders"},
    {"Shader cache", "Cache des shaders", "Shader-Cache", "Caché de shaders", "Cache shader", "Cache de shaders"},
    {"Import textures", "Importer les textures", "Texturen importieren", "Importar texturas", "Importa texture", "Importar texturas"},
    {"Export textures", "Exporter les textures", "Texturen exportieren", "Exportar texturas", "Esporta texture", "Exportar texturas"},
    {"Export as PNG", "Exporter en PNG", "Als PNG exportieren", "Exportar como PNG", "Esporta come PNG", "Exportar como PNG"},
    {"FPS hack", "Hack FPS", "FPS-Hack", "Hack de FPS", "Hack FPS", "Hack de FPS"},
    {"Performance overlay", "Overlay de performances", "Leistungs-Overlay", "Superposición de rendimiento", "Overlay prestazioni", "Sobreposição de desempenho"},
    {"Overlay detail", "Détails de l'overlay", "Overlay-Details", "Detalle de la superposición", "Dettaglio overlay", "Detalhe da sobreposição"},
    {"Overlay position", "Position de l'overlay", "Overlay-Position", "Posición de la superposición", "Posizione overlay", "Posição da sobreposição"},
    {"Audio volume", "Volume audio", "Audiolautstärke", "Volumen de audio", "Volume audio", "Volume do áudio"},
    {"NGS audio support", "Prise en charge audio NGS", "NGS-Audiounterstützung", "Compatibilidad de audio NGS", "Supporto audio NGS", "Suporte de áudio NGS"},
    {"System language", "Langue du système", "Systemsprache", "Idioma del sistema", "Lingua di sistema", "Idioma do sistema"},
    {"Enter button", "Bouton de validation", "Bestätigungstaste", "Botón de confirmación", "Tasto di conferma", "Botão de confirmação"},
    {"Date format", "Format de date", "Datumsformat", "Formato de fecha", "Formato data", "Formato de data"},
    {"Time format", "Format de l'heure", "Zeitformat", "Formato de hora", "Formato ora", "Formato da hora"},
    {"PS TV mode", "Mode PS TV", "PS-TV-Modus", "Modo PS TV", "Modalità PS TV", "Modo PS TV"},
    {"HTTP enable", "Activer HTTP", "HTTP aktivieren", "Activar HTTP", "Abilita HTTP", "Ativar HTTP"},
    {"HTTP timeout attempts", "Tentatives avant expiration HTTP", "HTTP-Timeout-Versuche", "Intentos de espera HTTP", "Tentativi timeout HTTP", "Tentativas de timeout HTTP"},
    {"HTTP timeout sleep (ms)", "Attente du délai HTTP (ms)", "HTTP-Timeout-Pause (ms)", "Espera de tiempo HTTP (ms)", "Pausa timeout HTTP (ms)", "Pausa de timeout HTTP (ms)"},
    {"HTTP read-end attempts", "Tentatives de fin de lecture HTTP", "HTTP-Leseende-Versuche", "Intentos de fin de lectura HTTP", "Tentativi fine lettura HTTP", "Tentativas de fim de leitura HTTP"},
    {"HTTP read-end sleep (ms)", "Attente de fin de lecture HTTP (ms)", "HTTP-Leseende-Pause (ms)", "Espera de fin de lectura HTTP (ms)", "Pausa fine lettura HTTP (ms)", "Pausa de fim de leitura HTTP (ms)"},
    {"PSN signed in", "Connexion PSN active", "Bei PSN angemeldet", "Sesión PSN iniciada", "Accesso PSN effettuato", "Sessão PSN iniciada"},
    {"Ad-hoc address index", "Index d'adresse ad hoc", "Ad-hoc-Adressindex", "Índice de dirección ad hoc", "Indice indirizzo ad hoc", "Índice de endereço ad hoc"},
    {"Disable motion", "Désactiver les mouvements", "Bewegung deaktivieren", "Desactivar movimiento", "Disabilita movimento", "Desativar movimento"},
    {"Stick sensitivity", "Sensibilité du stick", "Stick-Empfindlichkeit", "Sensibilidad del stick", "Sensibilità stick", "Sensibilidade do analógico"},
    {"Stick deadzone (%)", "Zone morte du stick (%)", "Stick-Totzone (%)", "Zona muerta del stick (%)", "Zona morta stick (%)", "Zona morta do analógico (%)"},
    {"Rear touch modifier", "Modificateur tactile arrière", "Rückseiten-Touch-Modifikator", "Modificador táctil trasero", "Modificatore touch posteriore", "Modificador tátil traseiro"},
    {"Switch A maps to", "Bouton Switch A associé à", "Switch A entspricht", "Switch A se asigna a", "Switch A corrisponde a", "Switch A corresponde a"},
    {"Switch B maps to", "Bouton Switch B associé à", "Switch B entspricht", "Switch B se asigna a", "Switch B corrisponde a", "Switch B corresponde a"},
    {"Switch X maps to", "Bouton Switch X associé à", "Switch X entspricht", "Switch X se asigna a", "Switch X corrisponde a", "Switch X corresponde a"},
    {"Switch Y maps to", "Bouton Switch Y associé à", "Switch Y entspricht", "Switch Y se asigna a", "Switch Y corrisponde a", "Switch Y corresponde a"},
    {"Launcher rotation", "Rotation du lanceur", "Launcher-Drehung", "Rotación del lanzador", "Rotazione launcher", "Rotação do launcher"},
    {"SteamGridDB API key", "Clé API SteamGridDB", "SteamGridDB-API-Schlüssel", "Clave API de SteamGridDB", "Chiave API SteamGridDB", "Chave API SteamGridDB"},
    {"0.5x", "0.5x", "0.5x", "0.5x", "0.5x", "0.5x"},
    {"1.0x", "1.0x", "1.0x", "1.0x", "1.0x", "1.0x"},
    {"1.5x", "1.5x", "1.5x", "1.5x", "1.5x", "1.5x"},
    {"2.0x", "2.0x", "2.0x", "2.0x", "2.0x", "2.0x"},
    {"2x", "2x", "2x", "2x", "2x", "2x"},
    {"3.0x", "3.0x", "3.0x", "3.0x", "3.0x", "3.0x"},
    {"4x", "4x", "4x", "4x", "4x", "4x"},
    {"8x", "8x", "8x", "8x", "8x", "8x"},
    {"16x", "16x", "16x", "16x", "16x", "16x"},
    {"1", "1", "1", "1", "1", "1"},
    {"2", "2", "2", "2", "2", "2"},
    {"3", "3", "3", "3", "3", "3"},
    {"4", "4", "4", "4", "4", "4"},
    {"5", "5", "5", "5", "5", "5"},
    {"6", "6", "6", "6", "6", "6"},
    {"7", "7", "7", "7", "7", "7"},
    {"8", "8", "8", "8", "8", "8"},
    {"Automatic", "Automatique", "Automatisch", "Automático", "Automatico", "Automático"},
    {"Auto + manual", "Auto + manuel", "Auto + manuell", "Auto + manual", "Auto + manuale", "Auto + manual"},
    {"Manual", "Manuel", "Manuell", "Manual", "Manuale", "Manual"},
    {"Nearest", "Nearest", "Nearest", "Nearest", "Nearest", "Nearest"},
    {"Bilinear", "Bilinear", "Bilinear", "Bilinear", "Bilineare", "Bilinear"},
    {"Bicubic", "Bicubique", "Bikubisch", "Bicúbico", "Bicubico", "Bicúbico"},
    {"FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA"},
    {"FSR", "FSR", "FSR", "FSR", "FSR", "FSR"},
    {"Double buffer", "Double buffer", "Doppelpuffer", "Búfer doble", "Doppio buffer", "Buffer duplo"},
    {"0 degrees", "0 degré", "0 Grad", "0 grados", "0 gradi", "0 graus"},
    {"90 degrees", "90 degrés", "90 Grad", "90 grados", "90 gradi", "90 graus"},
    {"180 degrees", "180 degrés", "180 Grad", "180 grados", "180 gradi", "180 graus"},
    {"270 degrees", "270 degrés", "270 Grad", "270 grados", "270 gradi", "270 graus"},
    {"XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)"},
    {"Glow", "Lueur", "Leuchten", "Brillo", "Bagliore", "Brilho"},
    {"Bubbles", "Bulles", "Blasen", "Burbujas", "Bolle", "Bolhas"},
    {"Classic", "Classique", "Klassisch", "Clásico", "Classico", "Clássico"},
    {"OLED black", "Noir OLED", "OLED-Schwarz", "Negro OLED", "Nero OLED", "Preto OLED"},
    {"Quarter", "Quart", "Viertel", "Cuarto", "Quarto", "Quarto"},
    {"Half", "Demi", "Halb", "Mitad", "Metà", "Metade"},
    {"Minimum", "Minimum", "Minimum", "Mínimo", "Minimo", "Mínimo"},
    {"Low", "Faible", "Niedrig", "Bajo", "Basso", "Baixo"},
    {"Medium", "Moyen", "Mittel", "Medio", "Medio", "Médio"},
    {"Maximum", "Maximum", "Maximum", "Máximo", "Massimo", "Máximo"},
    {"Top left", "En haut à gauche", "Oben links", "Arriba a la izquierda", "In alto a sinistra", "Superior esquerdo"},
    {"Top center", "En haut au centre", "Oben mittig", "Arriba en el centro", "In alto al centro", "Superior central"},
    {"Top right", "En haut à droite", "Oben rechts", "Arriba a la derecha", "In alto a destra", "Superior direito"},
    {"Bottom left", "En bas à gauche", "Unten links", "Abajo a la izquierda", "In basso a sinistra", "Inferior esquerdo"},
    {"Bottom center", "En bas au centre", "Unten mittig", "Abajo en el centro", "In basso al centro", "Inferior central"},
    {"Bottom right", "En bas à droite", "Unten rechts", "Abajo a la derecha", "In basso a destra", "Inferior direito"},
    {"Cross", "Croix", "Kreuz", "Cruz", "Croce", "Cruz"},
    {"Circle", "Cercle", "Kreis", "Círculo", "Cerchio", "Círculo"},
    {"Triangle", "Triangle", "Dreieck", "Triángulo", "Triangolo", "Triângulo"},
    {"Square", "Carré", "Quadrat", "Cuadrado", "Quadrato", "Quadrado"},
    {"ZL + touchscreen", "ZL + écran tactile", "ZL + Touchscreen", "ZL + pantalla táctil", "ZL + touchscreen", "ZL + ecrã tátil"},
    {"ZR + touchscreen", "ZR + écran tactile", "ZR + Touchscreen", "ZR + pantalla táctil", "ZR + touchscreen", "ZR + ecrã tátil"},
    {"12-hour", "12 heures", "12 Stunden", "12 horas", "12 ore", "12 horas"},
    {"24-hour", "24 heures", "24 Stunden", "24 horas", "24 ore", "24 horas"},
    {"YYYY/MM/DD", "AAAA/MM/JJ", "JJJJ/MM/TT", "AAAA/MM/DD", "AAAA/MM/GG", "AAAA/MM/DD"},
    {"DD/MM/YYYY", "JJ/MM/AAAA", "TT/MM/JJJJ", "DD/MM/AAAA", "GG/MM/AAAA", "DD/MM/AAAA"},
    {"MM/DD/YYYY", "MM/JJ/AAAA", "MM/TT/JJJJ", "MM/DD/AAAA", "MM/GG/AAAA", "MM/DD/AAAA"},
    {"Japanese", "Japonais", "Japanisch", "Japonés", "Giapponese", "Japonês"},
    {"English", "Anglais", "Englisch", "Inglés", "Inglese", "Inglês"},
    {"English (US)", "Anglais (États-Unis)", "Englisch (USA)", "Inglés (EE. UU.)", "Inglese (USA)", "Inglês (EUA)"},
    {"English (UK)", "Anglais (Royaume-Uni)", "Englisch (GB)", "Inglés (Reino Unido)", "Inglese (Regno Unito)", "Inglês (Reino Unido)"},
    {"French", "Français", "Französisch", "Francés", "Francese", "Francês"},
    {"Spanish", "Espagnol", "Spanisch", "Español", "Spagnolo", "Espanhol"},
    {"German", "Allemand", "Deutsch", "Alemán", "Tedesco", "Alemão"},
    {"Italian", "Italien", "Italienisch", "Italiano", "Italiano", "Italiano"},
    {"Dutch", "Néerlandais", "Niederländisch", "Neerlandés", "Olandese", "Neerlandês"},
    {"Portuguese", "Portugais", "Portugiesisch", "Portugués", "Portoghese", "Português"},
    {"Portuguese (BR)", "Portugais (Brésil)", "Portugiesisch (Brasilien)", "Portugués (Brasil)", "Portoghese (Brasile)", "Português (Brasil)"},
    {"Russian", "Russe", "Russisch", "Ruso", "Russo", "Russo"},
    {"Korean", "Coréen", "Koreanisch", "Coreano", "Coreano", "Coreano"},
    {"Chinese (Trad.)", "Chinois (traditionnel)", "Chinesisch (trad.)", "Chino (trad.)", "Cinese (trad.)", "Chinês (trad.)"},
    {"Chinese (Simp.)", "Chinois (simplifié)", "Chinesisch (vereinf.)", "Chino (simpl.)", "Cinese (sempl.)", "Chinês (simpl.)"},
    {"Finnish", "Finnois", "Finnisch", "Finés", "Finlandese", "Finlandês"},
    {"Swedish", "Suédois", "Schwedisch", "Sueco", "Svedese", "Sueco"},
    {"Danish", "Danois", "Dänisch", "Danés", "Danese", "Dinamarquês"},
    {"Norwegian", "Norvégien", "Norwegisch", "Noruego", "Norvegese", "Norueguês"},
    {"Polish", "Polonais", "Polnisch", "Polaco", "Polacco", "Polaco"},
    {"Turkish", "Turc", "Türkisch", "Turco", "Turco", "Turco"},
    {"Français", "Français", "Französisch", "Francés", "Francese", "Francês"},
    {"Deutsch", "Allemand", "Deutsch", "Alemán", "Tedesco", "Alemão"},
    {"Español", "Espagnol", "Spanisch", "Español", "Spagnolo", "Espanhol"},
    {"Italiano", "Italien", "Italienisch", "Italiano", "Italiano", "Italiano"},
    {"Português", "Portugais", "Portugiesisch", "Portugués", "Portoghese", "Português"},
    {"Global Vita3K setting", "Réglage global Vita3K", "Globale Vita3K-Einstellung", "Ajuste global de Vita3K", "Impostazione globale Vita3K", "Definição global do Vita3K"},
    {"Per-game override", "Réglage propre au jeu", "Spielspezifische Überschreibung", "Ajuste específico del juego", "Override specifico del gioco", "Substituição específica do jogo"},
    {"Launcher setting", "Réglage du lanceur", "Launcher-Einstellung", "Ajuste del lanzador", "Impostazione launcher", "Definição do launcher"},
    {"Launcher action", "Action du lanceur", "Launcher-Aktion", "Acción del lanzador", "Azione launcher", "Ação do launcher"},
    {"Launcher settings", "Réglages du lanceur", "Launcher-Einstellungen", "Ajustes del lanzador", "Impostazioni launcher", "Definições do launcher"},
    {"Launcher section", "Section du lanceur", "Launcher-Bereich", "Sección del lanzador", "Sezione launcher", "Secção do launcher"},
    {"Launcher tools", "Outils du lanceur", "Launcher-Werkzeuge", "Herramientas del lanzador", "Strumenti launcher", "Ferramentas do launcher"},
    {"Storage category", "Catégorie de stockage", "Speicherkategorie", "Categoría de almacenamiento", "Categoria archiviazione", "Categoria de armazenamento"},
    {"Storage tools", "Outils de stockage", "Speicherwerkzeuge", "Herramientas de almacenamiento", "Strumenti di archiviazione", "Ferramentas de armazenamento"},
    {"SMB configuration", "Configuration SMB", "SMB-Konfiguration", "Configuración SMB", "Configurazione SMB", "Configuração SMB"},
    {"Vita3K setting category", "Catégorie de réglages Vita3K", "Vita3K-Einstellungskategorie", "Categoría de ajustes de Vita3K", "Categoria impostazioni Vita3K", "Categoria de definições do Vita3K"},
    {"Global emulator settings", "Réglages globaux de l'émulateur", "Globale Emulator-Einstellungen", "Ajustes globales del emulador", "Impostazioni globali emulatore", "Definições globais do emulador"},
    {"Game override", "Réglage du jeu", "Spielüberschreibung", "Ajuste del juego", "Override del gioco", "Substituição do jogo"},
    {"Safely eject", "Éjecter en toute sécurité", "Sicher auswerfen", "Expulsar de forma segura", "Espelli in sicurezza", "Ejetar com segurança"},
    {"USB drive ejected safely", "Périphérique USB éjecté en toute sécurité", "USB-Laufwerk sicher ausgeworfen", "Unidad USB expulsada de forma segura", "Unità USB espulsa in sicurezza", "Unidade USB ejetada em segurança"},
    {"Cancel", "Annuler", "Abbrechen", "Cancelar", "Annulla", "Cancelar"},
    {"Cancelling...", "Annulation...", "Wird abgebrochen...", "Cancelando...", "Annullamento...", "A cancelar..."},
    {"Page", "Page", "Seite", "Página", "Pagina", "Página"},
    {"Sort:", "Tri :", "Sortierung:", "Orden:", "Ordine:", "Ordenação:"},
    {"Recently played", "Joués récemment", "Kürzlich gespielt", "Jugados recientemente", "Giocati di recente", "Jogados recentemente"},
    {"Recently added", "Ajoutés récemment", "Kürzlich hinzugefügt", "Añadidos recientemente", "Aggiunti di recente", "Adicionados recentemente"},
    {"Launch", "Lancer", "Starten", "Iniciar", "Avvia", "Iniciar"},
    {"Rename game", "Renommer le jeu", "Spiel umbenennen", "Renombrar juego", "Rinomina gioco", "Renomear jogo"},
    {"Create HOME shortcut", "Créer un raccourci HOME", "HOME-Verknüpfung erstellen", "Crear acceso directo HOME", "Crea collegamento HOME", "Criar atalho HOME"},
    {"Favorites & collections", "Favoris et collections", "Favoriten & Sammlungen", "Favoritos y colecciones", "Preferiti e raccolte", "Favoritos e coleções"},
    {"Clear cache", "Vider le cache", "Cache leeren", "Borrar caché", "Svuota cache", "Limpar cache"},
    {"Clear per-game settings", "Effacer les paramètres par jeu", "Spielspezifische Einstellungen löschen", "Borrar ajustes por juego", "Cancella impostazioni per gioco", "Limpar definições por jogo"},
    {"Delete game (remove from SD)", "Supprimer le jeu (retirer de la SD)", "Spiel löschen (von SD entfernen)", "Eliminar juego (quitar de la SD)", "Elimina gioco (rimuovi dalla SD)", "Eliminar jogo (remover do SD)"},
    {"Cover import failed", "Échec de l'importation de la jaquette", "Cover-Import fehlgeschlagen", "Error al importar la carátula", "Importazione copertina non riuscita", "Falha ao importar a capa"},
    {"Cover removal failed", "Échec de la suppression de la jaquette", "Cover-Entfernung fehlgeschlagen", "Error al quitar la carátula", "Rimozione copertina non riuscita", "Falha ao remover a capa"},
    {"Create a user", "Créer un utilisateur", "Benutzer erstellen", "Crear un usuario", "Crea un utente", "Criar um utilizador"},
    {"Manual module list", "Liste manuelle des modules", "Manuelle Modulliste", "Lista manual de módulos", "Elenco moduli manuale", "Lista manual de módulos"},
    {"None", "Aucun", "Keine", "Ninguno", "Nessuno", "Nenhum"},
    {"Press A to continue", "Appuyez sur A pour continuer", "Zum Fortfahren A drücken", "Pulsa A para continuar", "Premi A per continuare", "Prima A para continuar"},
    {"Selected", "Sélectionné", "Ausgewählt", "Seleccionado", "Selezionato", "Selecionado"},
    {"Touch anywhere to close", "Touchez l'écran pour fermer", "Zum Schließen tippen", "Toca en cualquier parte para cerrar", "Tocca lo schermo per chiudere", "Toque em qualquer lugar para fechar"},
    {"Users", "Utilisateurs", "Benutzer", "Usuarios", "Utenti", "Utilizadores"},
    {"Vita3K creates a default user on first launch", "Vita3K crée un utilisateur par défaut au premier lancement", "Vita3K erstellt beim ersten Start einen Standardbenutzer", "Vita3K crea un usuario predeterminado al iniciarse por primera vez", "Vita3K crea un utente predefinito al primo avvio", "O Vita3K cria um utilizador predefinido no primeiro arranque"},
    {"no users yet", "aucun utilisateur", "noch keine Benutzer", "aún no hay usuarios", "nessun utente", "ainda sem utilizadores"},
    {"Actions", "Actions", "Aktionen", "Acciones", "Azioni", "Ações"},
    {"Check Again", "Revérifier", "Erneut prüfen", "Buscar de nuevo", "Ricontrolla", "Verificar novamente"},
    {"Clear all", "Tout effacer", "Alle löschen", "Borrar todo", "Cancella tutto", "Limpar tudo"},
    {"Delete", "Supprimer", "Löschen", "Eliminar", "Elimina", "Eliminar"},
    {"Done", "Terminé", "Fertig", "Listo", "Fatto", "Concluído"},
    {"Download", "Télécharger", "Laden", "Descargar", "Scarica", "Transferir"},
    {"Edit", "Modifier", "Bearbeiten", "Editar", "Modifica", "Editar"},
    {"Filter", "Filtrer", "Filter", "Filtrar", "Filtra", "Filtrar"},
    {"Game Menu", "Menu du jeu", "Spielmenü", "Menú del juego", "Menu gioco", "Menu do jogo"},
    {"Help", "Aide", "Hilfe", "Ayuda", "Aiuto", "Ajuda"},
    {"Install & Exit", "Installer et quitter", "Installieren & beenden", "Instalar y salir", "Installa ed esci", "Instalar e sair"},
    {"Open", "Ouvrir", "Öffnen", "Abrir", "Apri", "Abrir"},
    {"Open / Select", "Ouvrir / Sélectionner", "Öffnen / Auswählen", "Abrir / Seleccionar", "Apri / Seleziona", "Abrir / Selecionar"},
    {"Paste", "Coller", "Einfügen", "Pegar", "Incolla", "Colar"},
    {"Quit", "Quitter", "Beenden", "Salir", "Esci", "Sair"},
    {"Rename", "Renommer", "Umbenennen", "Renombrar", "Rinomina", "Renomear"},
    {"Select", "Sélectionner", "Auswählen", "Seleccionar", "Seleziona", "Selecionar"},
    {"Sort", "Trier", "Sortieren", "Ordenar", "Ordina", "Ordenar"},
    {"Toggle", "Basculer", "Umschalten", "Alternar", "Cambia", "Alternar"},
    {"Use artwork", "Utiliser l'illustration", "Artwork verwenden", "Usar ilustración", "Usa immagine", "Usar imagem"},
    {"Use icon", "Utiliser l'icône", "Icon verwenden", "Usar icono", "Usa icona", "Usar ícone"},
    {"Choose an icon", "Choisir une icône", "Icon auswählen", "Elegir un icono", "Scegli un'icona", "Escolher um ícone"},
    {"Choose cover artwork", "Choisir une jaquette", "Cover auswählen", "Elegir carátula", "Scegli la copertina", "Escolher a imagem da capa"},
    {"Choose import storage", "Choisir le stockage d'importation", "Importspeicher auswählen", "Elegir almacenamiento", "Scegli la memoria di importazione", "Escolher o armazenamento de importação"},
    {"Creating HOME shortcut", "Création du raccourci HOME", "HOME-Verknüpfung wird erstellt", "Creando acceso directo HOME", "Creazione collegamento HOME", "A criar o atalho HOME"},
    {"File transfer", "Transfert de fichiers", "Dateiübertragung", "Transferencia de archivos", "Trasferimento file", "Transferência de ficheiros"},
    {"Firmware setup", "Configuration du micrologiciel", "Firmware-Einrichtung", "Configuración del firmware", "Configurazione firmware", "Configuração do firmware"},
    {"Game menu", "Menu du jeu", "Spielmenü", "Menú del juego", "Menu gioco", "Menu do jogo"},
    {"Vita3K-nx Update", "Mise à jour de Vita3K-nx", "Vita3K-nx-Update", "Actualización de Vita3K-nx", "Aggiornamento Vita3K-nx", "Atualização do Vita3K-nx"},
    {"Chooses which decrypted firmware modules are loaded natively instead of being emulated. It needs Modules mode set to something other than Automatic, and installed firmware. Picking nothing in Manual mode loads no modules at all, which is usually worse than Automatic.", "Choisit les modules de micrologiciel déchiffrés chargés nativement au lieu d'être émulés. Nécessite un Mode des modules autre qu'Automatique et un micrologiciel installé. Ne rien choisir en mode Manuel ne charge aucun module, ce qui est généralement pire qu'Automatique.", "Legt fest, welche entschlüsselten Firmware-Module nativ geladen statt emuliert werden. Erfordert einen Modulmodus außer Automatisch und installierte Firmware. Wird im Modus Manuell nichts gewählt, werden gar keine Module geladen, was meist schlechter ist als Automatisch.", "Elige qué módulos de firmware descifrados se cargan de forma nativa en vez de emularse. Requiere que Modo de módulos no sea Automático y tener firmware instalado. No elegir nada en modo Manual no carga ningún módulo, lo que suele ser peor que Automático.", "Sceglie quali moduli firmware decriptati vengono caricati in modo nativo anziché emulati. Richiede Modalità moduli su un valore diverso da Automatico e il firmware installato. In modalità Manuale, non selezionare nulla non carica alcun modulo, cosa di solito peggiore di Automatico.", "Escolhe que módulos de firmware desencriptados são carregados nativamente em vez de emulados. Requer o Modo de módulos diferente de Automático e firmware instalado. Não escolher nada no modo Manual não carrega qualquer módulo, o que costuma ser pior do que Automático."},
    {"Fills the whole screen instead of preserving the Vita 16:9.4 aspect. The image is distorted, and touch coordinates follow the stretched area.", "Remplit tout l'écran au lieu de conserver le rapport 16:9.4 de la Vita. L'image est déformée et les coordonnées tactiles suivent la zone étirée.", "Füllt den gesamten Bildschirm, statt das Vita-Seitenverhältnis 16:9.4 beizubehalten. Das Bild wird verzerrt, und die Touch-Koordinaten folgen dem gestreckten Bereich.", "Llena toda la pantalla en vez de conservar la relación 16:9.4 de Vita. La imagen se distorsiona y las coordenadas táctiles siguen el área estirada.", "Riempie tutto lo schermo invece di mantenere le proporzioni 16:9.4 della Vita. L'immagine risulta distorta e le coordinate touch seguono l'area allungata.", "Preenche todo o ecrã em vez de manter a proporção 16:9.4 da Vita. A imagem fica distorcida e as coordenadas táteis seguem a área esticada."},
    {"Shows or hides the coloured compatibility dot in the bottom-left corner of each cover. Download the database from Library & storage first.", "Affiche ou masque la pastille de compatibilité colorée dans le coin inférieur gauche de chaque jaquette. Téléchargez d'abord la base de compatibilité depuis Bibliothèque et stockage.", "Blendet den farbigen Kompatibilitätspunkt unten links auf jedem Cover ein oder aus. Lade die Datenbank zuvor unter Bibliothek & Speicher herunter.", "Muestra u oculta el punto de compatibilidad de color en la esquina inferior izquierda de cada carátula. Descarga antes la base de datos desde Biblioteca y almacenamiento.", "Mostra o nasconde il pallino colorato di compatibilità nell'angolo inferiore sinistro di ogni copertina. Scarica prima il database da Libreria e archiviazione.", "Mostra ou oculta o ponto colorido de compatibilidade no canto inferior esquerdo de cada capa. Transfira primeiro a base de dados em Biblioteca e armazenamento."},
    {"User profiles", "Profils utilisateur", "Benutzerprofile", "Perfiles de usuario", "Profili utente", "Perfis de utilizador"},
    {"Vita users", "Utilisateurs Vita", "Vita-Benutzer", "Usuarios de Vita", "Utenti Vita", "Utilizadores Vita"},
    {"Show compatibility badges", "Afficher les indicateurs de compatibilité", "Kompatibilitätsmarkierungen anzeigen", "Mostrar indicadores de compatibilidad", "Mostra indicatori di compatibilità", "Mostrar indicadores de compatibilidade"},
    {"Stretch to screen", "Étirer à l'écran", "Auf Bildschirm strecken", "Estirar a la pantalla", "Estendi a tutto schermo", "Esticar para o ecrã"},
    {"Controller", "Manette", "Controller", "Mando", "Controller", "Comando"},
    {"Emulation", "Émulation", "Emulation", "Emulación", "Emulazione", "Emulação"},
    {"Frame Generation", "Génération d'images", "Frame-Generierung", "Generación de fotogramas", "Generazione fotogrammi", "Geração de fotogramas"},
    {"GPU / Graphics", "GPU / Graphismes", "GPU / Grafik", "GPU / Gráficos", "GPU / Grafica", "GPU / Gráficos"},
    {"A SteamGridDB API key is required", "Une clé API SteamGridDB est requise", "Ein SteamGridDB-API-Schlüssel ist erforderlich", "Se necesita una clave API de SteamGridDB", "È richiesta una chiave API SteamGridDB", "É necessária uma chave API SteamGridDB"},
    {"All covers already downloaded", "Toutes les jaquettes sont déjà téléchargées", "Alle Cover bereits heruntergeladen", "Todas las carátulas ya están descargadas", "Tutte le copertine sono già scaricate", "Todas as capas já foram transferidas"},
    {"Cache cleared", "Cache vidé", "Cache geleert", "Caché borrada", "Cache svuotata", "Cache limpa"},
    {"Copied to clipboard", "Copié dans le presse-papiers", "In die Zwischenablage kopiert", "Copiado al portapapeles", "Copiato negli appunti", "Copiado para a área de transferência"},
    {"Could not change launcher orientation", "Impossible de changer l'orientation du lanceur", "Launcher-Ausrichtung konnte nicht geändert werden", "No se pudo cambiar la orientación del lanzador", "Impossibile cambiare l'orientamento del launcher", "Não foi possível alterar a orientação do launcher"},
    {"Could not create the user", "Impossible de créer l'utilisateur", "Benutzer konnte nicht erstellt werden", "No se pudo crear el usuario", "Impossibile creare l'utente", "Não foi possível criar o utilizador"},
    {"Could not reach the compatibility database", "Impossible de joindre la base de compatibilité", "Kompatibilitätsdatenbank nicht erreichbar", "No se pudo acceder a la base de datos de compatibilidad", "Impossibile raggiungere il database di compatibilità", "Não foi possível aceder à base de dados de compatibilidade"},
    {"Could not rename the user", "Impossible de renommer l'utilisateur", "Benutzer konnte nicht umbenannt werden", "No se pudo renombrar el usuario", "Impossibile rinominare l'utente", "Não foi possível renomear o utilizador"},
    {"Could not save the compatibility database", "Impossible d'enregistrer la base de compatibilité", "Kompatibilitätsdatenbank konnte nicht gespeichert werden", "No se pudo guardar la base de datos de compatibilidad", "Impossibile salvare il database di compatibilità", "Não foi possível guardar a base de dados de compatibilidade"},
    {"Cover download failed", "Échec du téléchargement de la jaquette", "Cover-Download fehlgeschlagen", "Error al descargar la carátula", "Download della copertina non riuscito", "Falha ao transferir a capa"},
    {"Cover downloaded", "Jaquette téléchargée", "Cover heruntergeladen", "Carátula descargada", "Copertina scaricata", "Capa transferida"},
    {"Firmware downloaded - installing...", "Micrologiciel téléchargé - installation...", "Firmware heruntergeladen - wird installiert...", "Firmware descargado - instalando...", "Firmware scaricato - installazione...", "Firmware transferido - a instalar..."},
    {"Game deleted", "Jeu supprimé", "Spiel gelöscht", "Juego eliminado", "Gioco eliminato", "Jogo eliminado"},
    {"HOME shortcut installed", "Raccourci HOME installé", "HOME-Verknüpfung installiert", "Acceso directo HOME instalado", "Collegamento HOME installato", "Atalho HOME instalado"},
    {"Installing firmware from local files...", "Installation du micrologiciel depuis des fichiers locaux...", "Firmware wird aus lokalen Dateien installiert...", "Instalando firmware desde archivos locales...", "Installazione firmware da file locali...", "A instalar o firmware a partir de ficheiros locais..."},
    {"Maximum of 24 pinned folders", "24 dossiers épinglés au maximum", "Maximal 24 angeheftete Ordner", "Máximo de 24 carpetas ancladas", "Massimo 24 cartelle fissate", "Máximo de 24 pastas fixadas"},
    {"Maximum of 8 SMB shares", "8 partages SMB au maximum", "Maximal 8 SMB-Freigaben", "Máximo de 8 recursos SMB", "Massimo 8 condivisioni SMB", "Máximo de 8 partilhas SMB"},
    {"Move complete", "Déplacement terminé", "Verschieben abgeschlossen", "Movimiento completado", "Spostamento completato", "Movimentação concluída"},
    {"Move queued", "Déplacement en file d'attente", "Verschieben eingereiht", "Movimiento en cola", "Spostamento in coda", "Movimentação em fila"},
    {"No free user slot is available", "Aucun emplacement utilisateur libre", "Kein freier Benutzerplatz verfügbar", "No hay ningún espacio de usuario libre", "Nessuno slot utente disponibile", "Não há espaços de utilizador livres"},
    {"No icon found - add a SteamGridDB key or download a cover first", "Aucune icône trouvée - ajoutez une clé SteamGridDB ou téléchargez une jaquette", "Kein Icon gefunden - füge zuerst einen SteamGridDB-Schlüssel hinzu oder lade ein Cover herunter", "No se encontró ningún icono - añade una clave de SteamGridDB o descarga antes una carátula", "Nessuna icona trovata - aggiungi una chiave SteamGridDB o scarica prima una copertina", "Nenhum ícone encontrado - adicione uma chave SteamGridDB ou transfira primeiro uma capa"},
    {"No network connection is available", "Aucune connexion réseau disponible", "Keine Netzwerkverbindung verfügbar", "No hay conexión de red disponible", "Nessuna connessione di rete disponibile", "Não há ligação de rede disponível"},
    {"Per-game settings cleared", "Paramètres par jeu effacés", "Spielspezifische Einstellungen gelöscht", "Ajustes por juego borrados", "Impostazioni per gioco cancellate", "Definições por jogo limpas"},
    {"Pick an icon first", "Choisissez d'abord une icône", "Wähle zuerst ein Icon", "Elige antes un icono", "Scegli prima un'icona", "Escolha primeiro um ícone"},
    {"Renamed", "Renommé", "Umbenannt", "Renombrado", "Rinominato", "Renomeado"},
    {"The compatibility database is already up to date", "La base de compatibilité est déjà à jour", "Die Kompatibilitätsdatenbank ist bereits aktuell", "La base de datos de compatibilidad ya está actualizada", "Il database di compatibilità è già aggiornato", "A base de dados de compatibilidade já está atualizada"},
    {"The downloaded compatibility database was unreadable", "La base de compatibilité téléchargée est illisible", "Die heruntergeladene Kompatibilitätsdatenbank war unlesbar", "No se pudo leer la base de datos de compatibilidad descargada", "Il database di compatibilità scaricato è illeggibile", "A base de dados de compatibilidade transferida era ilegível"},
    {"Transfer cancelled", "Transfert annulé", "Übertragung abgebrochen", "Transferencia cancelada", "Trasferimento annullato", "Transferência cancelada"},
    {"Transfer complete", "Transfert terminé", "Übertragung abgeschlossen", "Transferencia completada", "Trasferimento completato", "Transferência concluída"},
    {"USB drive can now be removed", "Le périphérique USB peut être retiré", "USB-Laufwerk kann jetzt entfernt werden", "Ya se puede retirar la unidad USB", "Ora puoi rimuovere l'unità USB", "A unidade USB já pode ser removida"},
    {"User created", "Utilisateur créé", "Benutzer erstellt", "Usuario creado", "Utente creato", "Utilizador criado"},
    {"User deleted", "Utilisateur supprimé", "Benutzer gelöscht", "Usuario eliminado", "Utente eliminato", "Utilizador eliminado"},
    {"User renamed", "Utilisateur renommé", "Benutzer umbenannt", "Usuario renombrado", "Utente rinominato", "Utilizador renomeado"},
    {"User selected", "Utilisateur sélectionné", "Benutzer ausgewählt", "Usuario seleccionado", "Utente selezionato", "Utilizador selecionado"},
    {"A HOME forwarder needs sigpatches on your CFW.", "Un forwarder HOME nécessite des sigpatches sur votre CFW.", "Ein HOME-Forwarder benötigt sigpatches auf deiner CFW.", "Un forwarder HOME necesita sigpatches en tu CFW.", "Un forwarder HOME richiede i sigpatches sul tuo CFW.", "Um forwarder HOME precisa de sigpatches no seu CFW."},
    {"All of this user's save data and trophies are deleted.", "Toutes les sauvegardes et tous les trophées de cet utilisateur sont supprimés.", "Alle Spielstände und Trophäen dieses Benutzers werden gelöscht.", "Se eliminan todos los datos de guardado y trofeos de este usuario.", "Tutti i dati di salvataggio e i trofei di questo utente vengono eliminati.", "Todos os dados guardados e troféus deste utilizador são eliminados."},
    {"Choose another destination or rename the folder first.", "Choisissez une autre destination ou renommez d'abord le dossier.", "Wähle ein anderes Ziel oder benenne den Ordner zuerst um.", "Elige otro destino o renombra antes la carpeta.", "Scegli un'altra destinazione o rinomina prima la cartella.", "Escolha outro destino ou renomeie primeiro a pasta."},
    {"Close files using this drive before ejecting.", "Fermez les fichiers utilisant ce périphérique avant de l'éjecter.", "Schließe Dateien auf diesem Laufwerk vor dem Auswerfen.", "Cierra los archivos que usen esta unidad antes de expulsarla.", "Chiudi i file che usano questa unità prima di espellerla.", "Feche os ficheiros que usam esta unidade antes de ejetar."},
    {"Deletes compiled shaders and shader logs.", "Supprime les shaders compilés et leurs journaux.", "Löscht kompilierte Shader und Shader-Logs.", "Elimina los shaders compilados y sus registros.", "Elimina gli shader compilati e i relativi log.", "Elimina os shaders compilados e os registos de shaders."},
    {"Games and files are not deleted.", "Les jeux et les fichiers ne sont pas supprimés.", "Spiele und Dateien werden nicht gelöscht.", "Los juegos y archivos no se eliminan.", "I giochi e i file non vengono eliminati.", "Os jogos e os ficheiros não são eliminados."},
    {"Import the companion license with the selected package?", "Importer la licence associée avec le paquet sélectionné ?", "Die zugehörige Lizenz mit dem gewählten Paket importieren?", "¿Importar la licencia asociada con el paquete seleccionado?", "Importare la licenza associata con il pacchetto selezionato?", "Importar a licença associada com o pacote selecionado?"},
    {"Installed games and their files are not touched.", "Les jeux installés et leurs fichiers ne sont pas modifiés.", "Installierte Spiele und ihre Dateien bleiben unverändert.", "Los juegos instalados y sus archivos no se modifican.", "I giochi installati e i loro file non vengono modificati.", "Os jogos instalados e os seus ficheiros não são alterados."},
    {"Make sure the console is online and DNS is reachable.", "Vérifiez que la console est en ligne et que le DNS est joignable.", "Stelle sicher, dass die Konsole online und DNS erreichbar ist.", "Asegúrate de que la consola esté en línea y el DNS sea accesible.", "Verifica che la console sia online e che il DNS sia raggiungibile.", "Certifique-se de que a consola está online e o DNS acessível."},
    {"No files on the server will be deleted.", "Aucun fichier du serveur ne sera supprimé.", "Auf dem Server werden keine Dateien gelöscht.", "No se eliminará ningún archivo del servidor.", "Nessun file sul server verrà eliminato.", "Nenhum ficheiro no servidor será eliminado."},
    {"No new installer job was started.", "Aucune nouvelle tâche d'installation n'a été lancée.", "Es wurde kein neuer Installationsvorgang gestartet.", "No se inició ninguna tarea de instalación.", "Nessuna nuova operazione di installazione è stata avviata.", "Não foi iniciada nenhuma nova tarefa de instalação."},
    {"Save data and game files are not changed.", "Les sauvegardes et les fichiers de jeu ne sont pas modifiés.", "Spielstände und Spieldateien werden nicht geändert.", "Los datos de guardado y los archivos del juego no se modifican.", "I dati di salvataggio e i file di gioco non vengono modificati.", "Os dados guardados e os ficheiros do jogo não são alterados."},
    {"The device may be disconnected.", "Le périphérique est peut-être déconnecté.", "Das Gerät ist möglicherweise getrennt.", "Puede que el dispositivo esté desconectado.", "Il dispositivo potrebbe essere scollegato.", "O dispositivo pode estar desligado."},
    {"The downloaded or imported cover will be deleted.", "La jaquette téléchargée ou importée sera supprimée.", "Das heruntergeladene oder importierte Cover wird gelöscht.", "Se eliminará la carátula descargada o importada.", "La copertina scaricata o importata verrà eliminata.", "A capa transferida ou importada será eliminada."},
    {"The existing file will be replaced.", "Le fichier existant sera remplacé.", "Die vorhandene Datei wird ersetzt.", "Se sustituirá el archivo existente.", "Il file esistente verrà sostituito.", "O ficheiro existente será substituído."},
    {"The file transfer could not be completed.", "Le transfert de fichiers n'a pas pu être terminé.", "Die Dateiübertragung konnte nicht abgeschlossen werden.", "No se pudo completar la transferencia de archivos.", "Non è stato possibile completare il trasferimento dei file.", "Não foi possível concluir a transferência de ficheiros."},
    {"The installed launcher was left unchanged where possible.", "Le lanceur installé a été laissé inchangé dans la mesure du possible.", "Der installierte Launcher blieb nach Möglichkeit unverändert.", "El lanzador instalado se dejó sin cambios cuando fue posible.", "Il launcher installato è rimasto invariato dove possibile.", "O launcher instalado foi mantido inalterado sempre que possível."},
    {"The launcher will use the game's embedded artwork when available.", "Le lanceur utilisera l'illustration intégrée du jeu si elle est disponible.", "Der Launcher verwendet das eingebettete Artwork des Spiels, sofern vorhanden.", "El lanzador usará la ilustración incluida en el juego cuando esté disponible.", "Il launcher userà l'immagine integrata del gioco, se disponibile.", "O launcher usará a imagem incorporada do jogo quando disponível."},
    {"The selected folder is outside this storage root.", "Le dossier sélectionné est hors de la racine de ce stockage.", "Der gewählte Ordner liegt außerhalb dieses Speicherstammverzeichnisses.", "La carpeta seleccionada está fuera de la raíz de este almacenamiento.", "La cartella selezionata è fuori dalla radice di questa memoria.", "A pasta selecionada está fora da raiz deste armazenamento."},
    {"This cannot be undone.", "Cette action est irréversible.", "Dies kann nicht rückgängig gemacht werden.", "Esta acción no se puede deshacer.", "L'operazione non può essere annullata.", "Esta ação não pode ser anulada."},
    {"This permanently deletes the game from the SD card.", "Cela supprime définitivement le jeu de la carte SD.", "Dies löscht das Spiel dauerhaft von der SD-Karte.", "Esto elimina el juego de la tarjeta SD de forma permanente.", "Il gioco viene eliminato definitivamente dalla scheda SD.", "Isto elimina permanentemente o jogo do cartão SD."},
    {"Try again, or copy the PUP files from SD, USB, or SMB instead.", "Réessayez ou copiez plutôt les fichiers PUP depuis SD, USB ou SMB.", "Versuche es erneut oder kopiere die PUP-Dateien stattdessen von SD, USB oder SMB.", "Reinténtalo o copia los archivos PUP desde SD, USB o SMB.", "Riprova oppure copia i file PUP da SD, USB o SMB.", "Tente novamente ou copie os ficheiros PUP de SD, USB ou SMB."},
    {"Unknown error", "Erreur inconnue", "Unbekannter Fehler", "Error desconocido", "Errore sconosciuto", "Erro desconhecido"},
    {"Importing", "Importation", "Importieren", "Importando", "Importazione", "A importar"},
    {"Preparing import...", "Préparation de l'importation...", "Import wird vorbereitet...", "Preparando la importación...", "Preparazione dell'importazione...", "A preparar a importação..."},
    {"Cancelling import...", "Annulation de l'importation...", "Import wird abgebrochen...", "Cancelando la importación...", "Annullamento dell'importazione...", "A cancelar a importação..."},
    {"Installed", "Installé", "Installiert", "Instalado", "Installato", "Instalado"},
    {"Missing", "Manquant", "Fehlt", "Ausente", "Mancante", "Em falta"},
    {"Configured", "Configuré", "Konfiguriert", "Configurado", "Configurato", "Configurado"},
    {"Not configured", "Non configuré", "Nicht konfiguriert", "Sin configurar", "Non configurato", "Não configurado"},
    {"Setting", "Paramètre", "Einstellung", "Ajuste", "Impostazione", "Definição"},
    {"Setting help", "Aide sur le paramètre", "Hilfe zur Einstellung", "Ayuda del ajuste", "Guida all'impostazione", "Ajuda da definição"},
    {"The selected cover could not be imported safely.", "La jaquette sélectionnée n'a pas pu être importée en toute sécurité.", "Das ausgewählte Cover konnte nicht sicher importiert werden.", "No se pudo importar la carátula seleccionada de forma segura.", "Non è stato possibile importare la copertina selezionata in modo sicuro.", "Não foi possível importar a capa selecionada em segurança."},
    {"user", "utilisateur", "Benutzer", "usuario", "utente", "utilizador"},
    {"users", "utilisateurs", "Benutzer", "usuarios", "utenti", "utilizadores"},
};

std::unordered_map<std::string, std::string> s_translations;
std::string s_preference = "system";
std::string s_code = "en";

std::string ResolveSystemLanguage()
{
  u64 current = 0;
  if (R_FAILED(setInitialize()))
    return "en";
  const Result get_result = setGetSystemLanguage(&current);
  std::string result = "en";
  if (R_SUCCEEDED(get_result))
  {
    const struct { ::SetLanguage language; const char* code; } candidates[] = {
        {SetLanguage_FR, "fr"}, {SetLanguage_FRCA, "fr"}, {SetLanguage_DE, "de"},
        {SetLanguage_ES, "es"}, {SetLanguage_ES419, "es"}, {SetLanguage_IT, "it"},
        {SetLanguage_PT, "pt"}, {SetLanguage_PTBR, "pt"},
    };
    for (const auto& candidate : candidates)
    {
      u64 code = 0;
      if (R_SUCCEEDED(setMakeLanguageCode(candidate.language, &code)) && code == current)
      {
        result = candidate.code;
        break;
      }
    }
  }
  setExit();
  return result;
}
}

void SetLanguage(std::string preference)
{
  if (FindLanguage(preference) < 0)
    preference = "system";
  s_preference = std::move(preference);
  s_code = s_preference == "system" ? ResolveSystemLanguage() : s_preference;
  s_translations.clear();
  int column = 0;
  if (s_code == "fr") column = 1;
  else if (s_code == "de") column = 2;
  else if (s_code == "es") column = 3;
  else if (s_code == "it") column = 4;
  else if (s_code == "pt") column = 5;
  if (!column)
    return;
  for (const Entry& entry : ENTRIES)
  {
    const char* translated[] = {entry.source, entry.fr, entry.de, entry.es, entry.it, entry.pt};
    if (translated[column] && *translated[column])
      s_translations.emplace(entry.source, translated[column]);
  }
}

std::string_view Translate(std::string_view source)
{
  const auto found = s_translations.find(std::string(source));
  return found == s_translations.end() ? source : std::string_view(found->second);
}

const std::string& Preference()
{
  return s_preference;
}

std::string DisplayName()
{
  const int index = FindLanguage(s_preference);
  if (index <= 0)
  {
    const int resolved = FindLanguage(s_code);
    return std::string(Translate("System")) + " (" +
           (resolved >= 0 ? LANGUAGE_LIST[resolved].name : "English") + ")";
  }
  return LANGUAGE_LIST[index].name;
}

const std::vector<Language>& Languages()
{
  static const std::vector<Language> result(LANGUAGE_LIST.begin(), LANGUAGE_LIST.end());
  return result;
}

int FindLanguage(std::string_view code)
{
  for (std::size_t index = 0; index < LANGUAGE_LIST.size(); ++index)
    if (code == LANGUAGE_LIST[index].code)
      return static_cast<int>(index);
  return -1;
}
}

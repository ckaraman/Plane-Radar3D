#!/bin/bash
# Radar USB köprüsü (Mac). Finder'da çift tıklayın ya da Terminal'de çalıştırın.
cd "$(dirname "$0")" || exit 1
if ! python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 7) else 1)' 2>/dev/null; then
  echo "python3 bulunamadı. macOS 'Komut satırı geliştirici araçları'nı kurmayı önerirse"
  echo "'Yükle' deyin, kurulum bitince bu dosyayı yeniden açın."
  echo
  read -r -p "Kapatmak için Enter'a basın..." _
  exit 1
fi
python3 radar_kopru.py "$@"
echo
read -r -p "Kapatmak için Enter'a basın..." _

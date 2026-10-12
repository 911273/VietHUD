# Road-name channel job (Pi)

Weekly (Sun 03:30) on the Pi `homebridge`: download Geofabrik's Vietnam OSM
extract when it changed, build `roadnames/` (whole country) and
`roadnames_lite154/` (Ha Noi area for the Lite) with
`tools/map_builder/publish_roadnames.py`, sign them with
`~/.viethud/manifest_signing_key.pem`, and push only those two folders to
GitHub main. Devices pick them up through `src/net/NamesUpdater.cpp` at their
weekly data check. OpenStreetMap data only — it shares nothing with any other
data job.

Install (from this folder, once):

    ssh pi4 mkdir -p /home/admin/viethud-names
    scp run.sh pi4:/home/admin/viethud-names/
    scp viethud-names.service viethud-names.timer pi4:/tmp/
    ssh pi4 'sudo mv /tmp/viethud-names.* /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable --now viethud-names.timer'

Run now: `ssh pi4 sudo systemctl start viethud-names.service`; log in
`/home/admin/viethud-names/logs/`.

# Fixtures ResNet50 (test d'inférence sur modèle entier)

Le scénario `tests/scenarios/types/test_IAModeleONNXResNet50.owl` importe un
ResNet50 complet (classification ImageNet) et vérifie que l'inférence OWL
reproduit une sortie de référence capturée hors OWL (onnxruntime), à une
tolérance flottante près et avec la même classe gagnante.

Deux des trois fixtures sont **volumineuses** et **non versionnées** : il faut
les récupérer une fois pour rejouer la vérification en local. En leur absence,
le scénario **s'ignore proprement** (il reste vert, sans rien prouver).

## Fichiers attendus (dans ce dossier)

| Fichier | Taille | Versionné | Contenu |
|---|---|---|---|
| `resnet50-v1-7.onnx` | ~100 Mo | non | Le modèle ResNet50 pré-entraîné, format ONNX. |
| `resnet50-v1-7_input_0.pb` | ~600 Ko | non | Entrée de référence `[1,3,224,224]` (TensorProto). |
| `resnet50-v1-7_output_0.pb` | ~4 Ko | **oui** | Logits de référence `[1,1000]` (oracle, versionné). |

## Récupération

Les trois fichiers proviennent d'une même archive du dépôt public de modèles
ONNX (`resnet50-v1-7.tar.gz`), servie via Git LFS.

### Piège LFS (à ne pas manquer)

Une URL `github.com/.../raw/...` peut renvoyer un fichier **tronqué** (le
contenu s'arrête avant la fin, sans erreur HTTP visible). Il faut :

1. Télécharger depuis l'hôte de résolution LFS
   (`media.githubusercontent.com/media/...`), pas le `raw` direct.
2. **Vérifier la complétude** : comparer la taille obtenue au `Content-Length`
   annoncé par le serveur. L'archive complète fait **95 435 189 octets**.

```sh
URL="https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/resnet/model/resnet50-v1-7.tar.gz"

# Content-Length attendu : 95435189
curl -sIL "$URL" | grep -i content-length

# Telechargement (avec reprise -C - si coupe en route)
curl -L -C - "$URL" -o resnet50-v1-7.tar.gz

# Verification d'integrite avant extraction
gzip -t resnet50-v1-7.tar.gz && echo "archive complete"
```

### Extraction et mise en place

L'archive se déploie en `resnet50-v1-7/` :

```
resnet50-v1-7/resnet50-v1-7.onnx
resnet50-v1-7/test_data_set_0/input_0.pb
resnet50-v1-7/test_data_set_0/output_0.pb
```

Copier les fichiers ici en les renommant :

```sh
tar -xzf resnet50-v1-7.tar.gz
cp resnet50-v1-7/resnet50-v1-7.onnx            resnet50-v1-7.onnx
cp resnet50-v1-7/test_data_set_0/input_0.pb    resnet50-v1-7_input_0.pb
# resnet50-v1-7_output_0.pb est deja versionne ; le recopier n'est pas necessaire.
```

## Rejouer la vérification

```sh
./build/owl.exe test tests/scenarios/types/test_IAModeleONNXResNet50.owl
```

Résultat attendu : 6 assertions au vert. Mesure de référence (onnxruntime) :
écart maximal ~6,7e-06 sur les 1000 logits, classe gagnante identique
(indice 489).

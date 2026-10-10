# Point the shell to the softfp VitaSDK (use: source scripts/env.sh)
export VITASDK="${VITASDK_SOFTFP:-$HOME/vitasdk-softfp}"
export PATH="$VITASDK/bin:$PATH"
echo "VITASDK=$VITASDK"

//! Talking to an OpenAI-compatible chat endpoint, OpenRouter by default.
//!
//! The `openrouter_rs` types stay inside this module: it is a 0.x crate that
//! ships often, and a leak would spread every future breaking change across the
//! whole program.

use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as BASE64;
use futures_util::stream::{self, BoxStream};
use futures_util::{Stream, StreamExt};
use openrouter_rs::OpenRouterClient;
use openrouter_rs::api::chat::{ChatCompletionRequest, ContentPart, Message};
use openrouter_rs::types::Role;
use tokio::time::timeout;

use crate::store::Settings;
use crate::translate;

/// A stream of translation fragments.
///
/// Boxed so that the image and text paths have one type between them.
pub type Deltas = BoxStream<'static, Result<String>>;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
/// Resets on every chunk, so a slow-but-still-streaming translation is not cut
/// short — only a connection that has gone completely silent trips it.
const READ_TIMEOUT: Duration = Duration::from_secs(20);

pub struct Provider {
    client: OpenRouterClient,
    model: String,
    image_system_prompt: String,
    text_system_prompt: String,
}

impl Provider {
    pub fn new(settings: &Settings, api_key: &str) -> Result<Self> {
        let client = OpenRouterClient::builder()
            .api_key(api_key)
            .base_url(settings.base_url())
            .build()
            .context("cannot build the client")?;
        Ok(Self {
            client,
            model: settings.model.clone(),
            image_system_prompt: translate::image_system_prompt(
                settings.target(),
                settings.fallback(),
            ),
            text_system_prompt: translate::text_system_prompt(
                settings.target(),
                settings.fallback(),
            ),
        })
    }

    /// Fails unless the configured model exists and accepts images.
    ///
    /// Run at startup: a model that cannot see is otherwise only discovered on
    /// the first screenshot, long after the mistake was made.
    pub async fn check_model(&self) -> Result<()> {
        let models = timeout(CONNECT_TIMEOUT, self.client.models().list())
            .await
            .map_err(|_| anyhow!("listing models timed out"))?
            .context("cannot list models")?;

        let Some(model) = models.iter().find(|m| m.id == self.model) else {
            bail!("no such model: {}", self.model);
        };
        if !accepts_images(model) {
            bail!("model {} does not accept image input", self.model);
        }
        Ok(())
    }

    /// Streams the translation of a PNG screenshot.
    ///
    /// The reply also carries a transcription of the source, since the image
    /// is the only copy of it the program has.
    pub async fn translate_image(&self, png: &[u8]) -> Result<Deltas> {
        let data_url = format!("data:image/png;base64,{}", BASE64.encode(png));
        self.stream(
            &self.image_system_prompt,
            vec![
                ContentPart::image_url(data_url),
                ContentPart::text("Translate the text in this image."),
            ],
        )
        .await
    }

    /// Streams the translation of plain text.
    ///
    /// The caller already has the exact source, so the reply is translation
    /// only — nothing to transcribe back.
    pub async fn translate_text(&self, text: &str) -> Result<Deltas> {
        self.stream(
            &self.text_system_prompt,
            vec![ContentPart::text(format!(
                "Translate the following text.\n\n{text}"
            ))],
        )
        .await
    }

    async fn stream(&self, system_prompt: &str, parts: Vec<ContentPart>) -> Result<Deltas> {
        let request = ChatCompletionRequest::builder()
            .model(&self.model)
            .messages(vec![
                Message::new(Role::System, system_prompt.to_owned()),
                Message::with_parts(Role::User, parts),
            ])
            .build()
            .context("cannot build the translation request")?;

        let stream = timeout(CONNECT_TIMEOUT, self.client.chat().stream(&request))
            .await
            .map_err(|_| anyhow!("connecting to the endpoint timed out"))?
            .context("cannot reach the endpoint")?;

        let deltas = stream.filter_map(|chunk| async move {
            match chunk {
                Ok(chunk) => {
                    let text = chunk.choices.first().and_then(|c| c.content())?;
                    (!text.is_empty()).then(|| Ok(text.to_owned()))
                }
                Err(err) => Some(Err(
                    anyhow::Error::new(err).context("translation stream failed")
                )),
            }
        });

        Ok(with_read_timeout(deltas).boxed())
    }
}

/// Fails the stream once `READ_TIMEOUT` passes with no new chunk, rather than
/// leaving the caller waiting on a connection that has gone silent.
fn with_read_timeout(
    deltas: impl Stream<Item = Result<String>> + Send + 'static,
) -> impl Stream<Item = Result<String>> + Send + 'static {
    stream::unfold(Some(Box::pin(deltas)), |state| async move {
        let mut deltas = state?;
        match timeout(READ_TIMEOUT, deltas.next()).await {
            Ok(Some(item)) => Some((item, Some(deltas))),
            Ok(None) => None,
            Err(_) => Some((Err(anyhow!("the connection went silent")), None)),
        }
    })
}

fn accepts_images(model: &openrouter_rs::api::models::Model) -> bool {
    let arch = &model.architecture;
    if let Some(modalities) = &arch.input_modalities {
        return modalities.iter().any(|m| m.eq_ignore_ascii_case("image"));
    }
    // Older entries only carry the combined form, e.g. `text+image->text`.
    arch.modality
        .as_deref()
        .is_some_and(|m| m.split("->").next().is_some_and(|i| i.contains("image")))
}

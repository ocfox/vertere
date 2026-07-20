//! Talking to OpenRouter.
//!
//! The `openrouter_rs` types stay inside this module: it is a 0.x crate that
//! ships often, and a leak would spread every future breaking change across the
//! whole program.

use anyhow::{Context, Result, bail};
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as BASE64;
use futures_util::StreamExt;
use futures_util::stream::BoxStream;
use openrouter_rs::OpenRouterClient;
use openrouter_rs::api::chat::{ChatCompletionRequest, ContentPart, Message};
use openrouter_rs::types::Role;

use crate::store::Settings;
use crate::translate;

/// A stream of translation fragments.
///
/// Boxed so that the image and text paths have one type between them.
pub type Deltas = BoxStream<'static, Result<String>>;

pub struct Provider {
    client: OpenRouterClient,
    model: String,
    system_prompt: String,
}

impl Provider {
    pub fn new(settings: &Settings, api_key: &str) -> Result<Self> {
        let client = OpenRouterClient::builder()
            .api_key(api_key)
            .build()
            .context("cannot build the OpenRouter client")?;
        Ok(Self {
            client,
            model: settings.model.clone(),
            system_prompt: translate::system_prompt(settings.target(), settings.fallback()),
        })
    }

    /// Fails unless the configured model exists and accepts images.
    ///
    /// Run at startup: a model that cannot see is otherwise only discovered on
    /// the first screenshot, long after the mistake was made.
    pub async fn check_model(&self) -> Result<()> {
        let models = self
            .client
            .models()
            .list()
            .await
            .context("cannot list OpenRouter models")?;

        let Some(model) = models.iter().find(|m| m.id == self.model) else {
            bail!("no such model on OpenRouter: {}", self.model);
        };
        if !accepts_images(model) {
            bail!("model {} does not accept image input", self.model);
        }
        Ok(())
    }

    /// Streams the translation of a PNG screenshot.
    pub async fn translate_image(&self, png: &[u8]) -> Result<Deltas> {
        let data_url = format!("data:image/png;base64,{}", BASE64.encode(png));
        self.stream(vec![
            ContentPart::image_url(data_url),
            ContentPart::text("Translate the text in this image."),
        ])
        .await
    }

    /// Streams the translation of plain text.
    pub async fn translate_text(&self, text: &str) -> Result<Deltas> {
        self.stream(vec![ContentPart::text(format!(
            "Translate the following text.\n\n{text}"
        ))])
        .await
    }

    async fn stream(&self, parts: Vec<ContentPart>) -> Result<Deltas> {
        let request = ChatCompletionRequest::builder()
            .model(&self.model)
            .messages(vec![
                Message::new(Role::System, self.system_prompt.clone()),
                Message::with_parts(Role::User, parts),
            ])
            .build()
            .context("cannot build the translation request")?;

        let stream = self
            .client
            .chat()
            .stream(&request)
            .await
            .context("cannot reach OpenRouter")?;

        Ok(stream
            .filter_map(|chunk| async move {
                match chunk {
                    Ok(chunk) => {
                        let text = chunk.choices.first().and_then(|c| c.content())?;
                        (!text.is_empty()).then(|| Ok(text.to_owned()))
                    }
                    Err(err) => Some(Err(
                        anyhow::Error::new(err).context("translation stream failed")
                    )),
                }
            })
            .boxed())
    }
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
